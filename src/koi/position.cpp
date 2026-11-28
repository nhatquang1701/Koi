#include "koi/position.hpp"

#include "koi/game_state.hpp"
#include "koi/detail/attack_tables.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace koi {

namespace {

constexpr std::uint8_t kWhiteKingSide = 0x1;
constexpr std::uint8_t kWhiteQueenSide = 0x2;
constexpr std::uint8_t kBlackKingSide = 0x4;
constexpr std::uint8_t kBlackQueenSide = 0x8;
constexpr std::uint8_t kAllCastling =
    kWhiteKingSide | kWhiteQueenSide | kBlackKingSide | kBlackQueenSide;
constexpr std::size_t kMaximumHistory = 256;

using Board = std::array<Piece, 64>;

// Sink adapters for materializing legal moves directly without an
// intermediate pseudo-legal buffer.
class VectorMoveSink {
public:
    explicit VectorMoveSink(std::vector<Move>& moves) noexcept : moves_(moves) {}

    template <typename... Arguments>
    [[nodiscard]] bool emplace_back(Arguments&&... arguments) {
        moves_.emplace_back(std::forward<Arguments>(arguments)...);
        return true;
    }

private:
    std::vector<Move>& moves_;
};

class SpanMoveSink {
public:
    explicit SpanMoveSink(std::span<Move> output) noexcept : output_(output) {}

    template <typename... Arguments>
    [[nodiscard]] bool emplace_back(Arguments&&... arguments) noexcept {
        if (count_ < output_.size()) {
            output_[count_] = Move(std::forward<Arguments>(arguments)...);
        }
        ++count_;
        return true;
    }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }

private:
    std::span<Move> output_;
    std::size_t count_ = 0;
};

class BoundedCountMoveSink {
public:
    explicit BoundedCountMoveSink(std::size_t limit) noexcept : limit_(limit) {}

    template <typename... Arguments>
    [[nodiscard]] bool emplace_back(Arguments&&...) noexcept {
        ++count_;
        return count_ < limit_;
    }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }

private:
    std::size_t limit_ = 0;
    std::size_t count_ = 0;
};

struct ZobristKeys {
    std::array<std::array<std::array<std::uint64_t, 64>, 7>, 2> pieces{};
    std::array<std::uint64_t, 16> castling{};
    std::array<std::uint64_t, 8> en_passant{};
    std::uint64_t black_to_move = 0;
};

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t extend_repetition_history_fingerprint(
    const std::uint64_t fingerprint, const std::uint64_t ancestor_key) noexcept {
    // This is an order-sensitive incremental combiner. The order is not
    // needed by repetition itself, but distinguishing different reversible
    // paths makes accidental TT context sharing less likely while snapshots
    // still make the operation exactly reversible on unmake.
    std::uint64_t seed = ancestor_key + 0x9E3779B97F4A7C15ULL;
    seed ^= fingerprint + 0xD6E8FEB86659FD93ULL +
        (seed << 6U) + (seed >> 2U);
    return splitmix64(seed) | 1ULL;
}

const ZobristKeys& zobrist() noexcept {
    static const ZobristKeys keys = [] {
        ZobristKeys result;
        std::uint64_t state = 0x4b4f492d76312e31ULL;
        for (std::size_t color = 0; color < 2; ++color) {
            for (std::size_t piece = 0; piece < 7; ++piece) {
                for (std::size_t square = 0; square < 64; ++square) {
                    result.pieces[color][piece][square] = splitmix64(state);
                }
            }
        }
        for (auto& value : result.castling) value = splitmix64(state);
        for (auto& value : result.en_passant) value = splitmix64(state);
        result.black_to_move = splitmix64(state);
        return result;
    }();
    return keys;
}

bool valid_square(int square) noexcept { return square >= 0 && square < 64; }
int file_of(int square) noexcept { return square & 7; }
int rank_of(int square) noexcept { return square >> 3; }

[[nodiscard]] constexpr bool is_ascii_space(const char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

Piece piece_from_char(char value) noexcept {
    const bool upper = (value >= 'A' && value <= 'Z');
    const Color color = upper ? Color::white : Color::black;
    const char lower = upper ? static_cast<char>(value - 'A' + 'a') : value;
    switch (lower) {
    case 'p': return {PieceType::pawn, color};
    case 'n': return {PieceType::knight, color};
    case 'b': return {PieceType::bishop, color};
    case 'r': return {PieceType::rook, color};
    case 'q': return {PieceType::queen, color};
    case 'k': return {PieceType::king, color};
    default: return {};
    }
}

char piece_to_char(Piece piece) noexcept {
    char value = '?';
    switch (piece.type) {
    case PieceType::pawn: value = 'p'; break;
    case PieceType::knight: value = 'n'; break;
    case PieceType::bishop: value = 'b'; break;
    case PieceType::rook: value = 'r'; break;
    case PieceType::queen: value = 'q'; break;
    case PieceType::king: value = 'k'; break;
    case PieceType::none: return '1';
    }
    return piece.color == Color::white ? static_cast<char>(value - 'a' + 'A') : value;
}

bool parse_uint(std::string_view value, std::uint32_t& output, bool allow_zero,
                std::uint32_t maximum) noexcept {
    if (value.empty()) return false;
    std::uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        (!allow_zero && parsed == 0) || parsed > maximum) {
        return false;
    }
    output = parsed;
    return true;
}

bool split_fields(std::string_view fen, std::array<std::string_view, 6>& fields) noexcept {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while (cursor < fen.size()) {
        while (cursor < fen.size() && is_ascii_space(fen[cursor])) ++cursor;
        if (cursor == fen.size()) break;
        if (count == fields.size()) return false;
        const std::size_t begin = cursor;
        while (cursor < fen.size() && !is_ascii_space(fen[cursor])) ++cursor;
        fields[count++] = fen.substr(begin, cursor - begin);
    }
    return count == fields.size();
}

// Inverse-make undo record: one 64-byte cache line per make. The make paths
// push one record before mutating the state, and unapply() reconstructs the
// pre-move state from it, so the 328-byte field-by-field Snapshot copy and
// the restore it fed are gone. The record holds only what a make cannot be
// inverted from the child state alone: the prior key, rule state, clocks,
// repetition state, and both king-mask slots (verbatim), plus the move and
// the captured piece so unapply() can mirror the piece moves through the
// same incremental add/remove helpers apply_unchecked() uses. Board,
// bitboards, occupancy, `occupied`, and `side` are reconstructed instead.
//
// The layout below is exact (alignof 8, no padding):
//   key 0, null_move 8, prior_suppressed 9, prior_halfmove 10,
//   prior_fullmove 12, prior_castling 14, prior_en_passant 15,
//   fingerprint 16, checkers 24, pinned 40, move 56, captured 60,
//   ep_victim 62, flags 63.
// `key` and `null_move` sit first on purpose: repetition_count() walks the
// ring backwards and reads exactly those two fields from every visited
// record, so one cache line still covers the whole probe.
struct UndoRecord {
    std::uint64_t key = 0;                     // prior position key, verbatim
    bool null_move = false;                    // repetition-scan kind
    bool prior_suppressed = false;             // prior repetition_history_suppressed
    std::uint16_t prior_halfmove = 0;          // uint16: a null move can store 256
    std::uint16_t prior_fullmove = 1;          // uint16: a black null move can store 32769
    std::uint8_t prior_castling = 0;
    std::uint8_t prior_en_passant = Square::kInvalid; // 64 = none, verbatim
    std::uint64_t fingerprint = 0;             // prior repetition_history_fingerprint
    std::array<std::uint64_t, 2> checkers{};   // prior checkers (both slots)
    std::array<std::uint64_t, 2> pinned{};     // prior pinned (both slots)
    Move move{};                               // from/to/promotion (4 B packed)
    Piece captured{};                          // piece removed (EP victim for EP)
    std::uint8_t ep_victim = Square::kInvalid; // EP victim square (else invalid)
    std::uint8_t flags = 0;                    // bit0 ep, bit1 castling, bit2 double_push, bit3 capture
};
static_assert(sizeof(UndoRecord) == 64);

// UndoRecord::flags bits, derived exactly as apply_unchecked() derives the
// booleans from the same pre-move state.
constexpr std::uint8_t kEpFlag = 0x1;
constexpr std::uint8_t kCastlingFlag = 0x2;
constexpr std::uint8_t kDoublePushFlag = 0x4;
constexpr std::uint8_t kCaptureFlag = 0x8;

struct NativeState {
    Board board{};
    Color side = Color::white;
    std::uint8_t castling = kAllCastling;
    Square en_passant{};
    std::uint16_t halfmove = 0;
    std::uint16_t fullmove = 1;
    std::uint64_t key = 0;
    std::uint64_t repetition_history_fingerprint = 0;
    bool repetition_history_suppressed = false;
    std::array<std::array<std::uint64_t, 7>, 2> piece_bitboards{};
    std::array<std::uint64_t, 2> occupancy{};
    std::uint64_t occupied = 0;
    std::array<std::uint64_t, 2> checkers{};
    std::array<std::uint64_t, 2> pinned{};
    std::array<UndoRecord, kMaximumHistory> history{};
    std::size_t history_size = 0;
};

bool has_legal_en_passant_capture(const NativeState& state) noexcept;
void refresh_king_masks(NativeState& state) noexcept;

// Pushes the next undo slot. The array has a fixed capacity so the search
// can make and unmake moves without allocating, but a game may legitimately
// run past it: a long replay then drops the oldest record and keeps the
// recent window that repetition detection and search need. Without this, a
// command listing more than `kMaximumHistory` moves would be rejected and
// the engine would keep answering the stale position. The overflow shift
// keeps the former ring semantics verbatim, with sizeof(UndoRecord)
// substituted: 255 x 64 = 16,320 B instead of 83,640 B.
UndoRecord* push_undo_record(NativeState& state) noexcept {
    if (state.history_size >= kMaximumHistory) [[unlikely]] {
        std::memmove(state.history.data(), state.history.data() + 1,
                     (kMaximumHistory - 1) * sizeof(UndoRecord));
        return &state.history[kMaximumHistory - 1];
    }
    return &state.history[state.history_size++];
}

// Normal moves record their undo slot directly inside record_and_apply_unchecked()
// using the already-decoded move facts.
void record_undo_null(NativeState& state) noexcept {
    UndoRecord* slot = push_undo_record(state);
    slot->key = state.key;
    slot->null_move = true;
    slot->prior_suppressed = state.repetition_history_suppressed;
    slot->prior_halfmove = state.halfmove;
    slot->prior_fullmove = state.fullmove;
    slot->prior_castling = state.castling;
    slot->prior_en_passant = state.en_passant.index();
    slot->fingerprint = state.repetition_history_fingerprint;
    slot->checkers = state.checkers;
    slot->pinned = state.pinned;
    slot->move = Move::no_move();
    slot->captured = Piece{};
    slot->ep_victim = Square::kInvalid;
    slot->flags = 0;
}

std::uint64_t bit(int square) noexcept {
    return valid_square(square) ? (std::uint64_t{1} << square) : 0;
}

std::uint64_t calculate_key(const NativeState& state) noexcept {
    const ZobristKeys& keys = zobrist();
    std::uint64_t result = keys.castling[state.castling & kAllCastling];
    for (int square = 0; square < 64; ++square) {
        const Piece piece = state.board[static_cast<std::size_t>(square)];
        if (!piece.empty()) {
            result ^= keys.pieces[piece.color == Color::white ? 0 : 1]
                [static_cast<std::size_t>(piece.type)][static_cast<std::size_t>(square)];
        }
    }
    if (has_legal_en_passant_capture(state)) {
        result ^= keys.en_passant[state.en_passant.index() & 7];
    }
    if (state.side == Color::black) result ^= keys.black_to_move;
    return result;
}

void rebuild_derived(NativeState& state) noexcept {
    state.piece_bitboards = {};
    state.occupancy = {};
    state.occupied = 0;
    for (int square = 0; square < 64; ++square) {
        const Piece piece = state.board[static_cast<std::size_t>(square)];
        if (piece.empty()) continue;
        const std::size_t color = piece.color == Color::white ? 0 : 1;
        state.piece_bitboards[color][static_cast<std::size_t>(piece.type)] |= bit(square);
        state.occupancy[color] |= bit(square);
    }
    state.occupied = state.occupancy[0] | state.occupancy[1];
    state.key = calculate_key(state);
    refresh_king_masks(state);
}

inline void remove_bitboard_piece_known(
    NativeState& state, Piece piece, int square) noexcept {
    const std::uint64_t square_bit = std::uint64_t{1} << square;
    const std::size_t color = piece.color == Color::white ? 0U : 1U;
    const std::size_t type = static_cast<std::size_t>(piece.type);
    state.piece_bitboards[color][type] &= ~square_bit;
    state.occupancy[color] &= ~square_bit;
}

inline void add_bitboard_piece_known(
    NativeState& state, Piece piece, int square) noexcept {
    const std::uint64_t square_bit = std::uint64_t{1} << square;
    const std::size_t color = piece.color == Color::white ? 0U : 1U;
    const std::size_t type = static_cast<std::size_t>(piece.type);
    state.piece_bitboards[color][type] |= square_bit;
    state.occupancy[color] |= square_bit;
}

void remove_bitboard_piece(
    NativeState& state, Piece piece, int square) noexcept {
    if (piece.empty() || !valid_square(square)) {
        return;
    }
    remove_bitboard_piece_known(state, piece, square);
}

void add_bitboard_piece(
    NativeState& state, Piece piece, int square) noexcept {
    if (piece.empty() || !valid_square(square)) {
        return;
    }
    add_bitboard_piece_known(state, piece, square);
}

int king_square(const NativeState& state, Color color) noexcept {
    const std::size_t color_index = color == Color::white ? 0U : 1U;
    const std::uint64_t kings =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
    if (kings == 0) return -1;
    return static_cast<int>(std::countr_zero(kings));
}

bool square_attacked(const NativeState& state, int target, Color attacker) noexcept {
    if (!valid_square(target)) return false;
    const std::size_t color_index = attacker == Color::white ? 0U : 1U;
    const bool white = attacker == Color::white;
    const std::uint64_t occupied = state.occupied;

    // Pawns attack `target` from the two squares a pawn of `attacker` would
    // have moved from; the reverse table answers that with one lookup.
    const std::uint64_t pawns =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::pawn)];
    if ((detail::pawn_attackers_of(target, white) & pawns) != 0) return true;

    // Knights and kings move without blocking, so their attack test is a pure
    // intersection of target-centric attack masks with the piece bitboards.
    if ((detail::knight_attacks(target) &
         state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::knight)]) != 0) {
        return true;
    }
    if ((detail::king_attacks(target) &
         state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)]) != 0) {
        return true;
    }

    // Sliding attacks are generated from the target through the real
    // occupancy, so the first blocker on each ray naturally stops the mask.
    const std::uint64_t bishops =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::bishop)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];
    if (bishops != 0 && (detail::bishop_pseudo_attacks(target) & bishops) != 0 &&
        (detail::bishop_attacks(target, occupied) & bishops) != 0) return true;
    const std::uint64_t rooks =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::rook)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];
    if (rooks != 0 && (detail::rook_pseudo_attacks(target) & rooks) != 0 &&
        (detail::rook_attacks(target, occupied) & rooks) != 0) return true;
    return false;
}

int attacker_count(const NativeState& state, int target, Color attacker) noexcept {
    if (!valid_square(target)) return 0;
    const std::size_t color_index = attacker == Color::white ? 0U : 1U;
    const bool white = attacker == Color::white;
    const std::uint64_t occupied = state.occupied;

    const std::uint64_t pawns =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::pawn)];
    const std::uint64_t knights =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::knight)];
    const std::uint64_t kings =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
    const std::uint64_t bishops =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::bishop)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];
    const std::uint64_t rooks =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::rook)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];

    int count = static_cast<int>(
        std::popcount(detail::pawn_attackers_of(target, white) & pawns));
    count += static_cast<int>(std::popcount(detail::knight_attacks(target) & knights));
    count += static_cast<int>(std::popcount(detail::king_attacks(target) & kings));
    if (bishops != 0 && (detail::bishop_pseudo_attacks(target) & bishops) != 0) {
        count += static_cast<int>(std::popcount(detail::bishop_attacks(target, occupied) & bishops));
    }
    if (rooks != 0 && (detail::rook_pseudo_attacks(target) & rooks) != 0) {
        count += static_cast<int>(std::popcount(detail::rook_attacks(target, occupied) & rooks));
    }
    return count;
}

bool is_checked(const NativeState& state, Color color) noexcept {
    const int king = king_square(state, color);
    return king < 0 || square_attacked(state, king, opposite(color));
}

// Recomputes the incremental king state for the side to move only: the
// checkers attacking its king and its own pieces pinned against an enemy
// slider.  apply_unchecked flips the side before callers ask about the mover,
// and every reader indexes the masks with the current side: the hoisted
// values in legal_moves, legal_moves_into_impl, and has_legal_move
// (position.cpp:1092, 1112, 1145), the re-read in
// resolve_move_legality (position.cpp:955), whose callers pass state.side as
// the mover, and the side-to-move in_check() probe (position.cpp:1389).
// NativeState is private to this translation unit, so no other
// reader exists.  The other colour's masks are intentionally left stale: no
// reader touches them, and record_undo()/unapply() carry both arrays through
// the undo record, so unmake restores whatever was current when the make
// recorded it.
void refresh_king_masks(NativeState& state) noexcept {
    const std::size_t color_index = state.side == Color::white ? 0U : 1U;
    state.checkers[color_index] = 0;
    state.pinned[color_index] = 0;
    const Color color = state.side;
    const int king = king_square(state, color);
    if (king < 0) return;
    const std::size_t enemy_index = 1 - color_index;
    const bool enemy_pawns_are_white = color == Color::black;
    const std::uint64_t enemy_pawns =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::pawn)];
    const std::uint64_t enemy_knights =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::knight)];
    const std::uint64_t enemy_kings =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::king)];
    const std::uint64_t enemy_bishops =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::bishop)] |
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::queen)];
    const std::uint64_t enemy_rooks =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::rook)] |
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::queen)];

    std::uint64_t checkers =
        (detail::pawn_attackers_of(king, enemy_pawns_are_white) & enemy_pawns) |
        (detail::knight_attacks(king) & enemy_knights) |
        (detail::king_attacks(king) & enemy_kings);

    // An enemy without a bishop/queen or rook/queen can never deliver slider
    // checks or pin through a blocker, so skip the sniper scan below while
    // `pinned` still holds zero.
    if (enemy_bishops == 0 && enemy_rooks == 0) {
        state.checkers[color_index] = checkers;
        return;
    }

    // Unified Stockfish-style sniper scan: find all aligned enemy sliders using
    // O(1) empty-board pseudo rays, then inspect between_mask(king, sniper).
    // Zero blockers between king and sniper is a direct check; a single
    // friendly non-king blocker is pinned. Avoids any magic lookup unless a
    // slider actually sits on a king ray.
    std::uint64_t snipers =
        (enemy_bishops != 0 ? (detail::bishop_pseudo_attacks(king) & enemy_bishops) : 0ULL) |
        (enemy_rooks != 0 ? (detail::rook_pseudo_attacks(king) & enemy_rooks) : 0ULL);
    if (snipers == 0) {
        state.checkers[color_index] = checkers;
        return;
    }
    const std::uint64_t friendly_non_king =
        state.occupancy[color_index] &
        ~state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
    std::uint64_t pinned = 0;
    while (snipers != 0) {
        const int sniper = static_cast<int>(std::countr_zero(snipers));
        snipers &= snipers - 1;
        const std::uint64_t blockers = detail::between_mask(king, sniper) & state.occupied;
        if (blockers == 0) {
            checkers |= bit(sniper);
        } else if ((blockers & (blockers - 1)) == 0 &&
                   (blockers & friendly_non_king) != 0) {
            pinned |= blockers;
        }
    }
    state.checkers[color_index] = checkers;
    state.pinned[color_index] = pinned;
}

// Pure read-only legality probe for en-passant captures.
bool has_legal_en_passant_capture(const NativeState& state) noexcept {
    if (state.en_passant.index() >= Square::kInvalid) {
        return false;
    }

    const int target = state.en_passant.index();
    const Color side = state.side;
    const std::size_t side_index = side == Color::white ? 0U : 1U;
    const std::size_t pawn_index = static_cast<std::size_t>(PieceType::pawn);
    const std::uint64_t own_pawns = state.piece_bitboards[side_index][pawn_index];
    std::uint64_t candidate_sources =
        own_pawns & detail::pawn_attackers_of(target, side == Color::white);
    if (candidate_sources == 0) {
        return false;
    }

    const int source_rank = rank_of(target) - (side == Color::white ? 1 : -1);
    const int captured_square = target + (side == Color::white ? -8 : 8);
    if (!valid_square(captured_square) || source_rank < 0 || source_rank >= 8 ||
        !state.board[static_cast<std::size_t>(target)].empty()) {
        return false;
    }

    const Piece captured = state.board[static_cast<std::size_t>(captured_square)];
    if (captured.type != PieceType::pawn || captured.color != opposite(side)) {
        return false;
    }

    const int king = king_square(state, side);
    if (king < 0) {
        return false;
    }
    const std::size_t enemy_index = 1U - side_index;
    const bool enemy_white = side == Color::black;
    const std::uint64_t captured_bit = std::uint64_t{1} << captured_square;
    const std::uint64_t target_bit = std::uint64_t{1} << target;
    const std::uint64_t enemy_pawns =
        state.piece_bitboards[enemy_index][pawn_index] & ~captured_bit;
    const std::uint64_t enemy_knights =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::knight)];
    const std::uint64_t enemy_kings =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::king)];
    if (((detail::pawn_attackers_of(king, enemy_white) & enemy_pawns) |
         (detail::knight_attacks(king) & enemy_knights) |
         (detail::king_attacks(king) & enemy_kings)) != 0) {
        return false;
    }
    const std::uint64_t enemy_bishops =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::bishop)] |
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::queen)];
    const std::uint64_t enemy_rooks =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::rook)] |
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::queen)];
    const std::uint64_t ep_diag_candidates =
        detail::bishop_pseudo_attacks(king) & enemy_bishops;
    const std::uint64_t ep_orth_candidates =
        detail::rook_pseudo_attacks(king) & enemy_rooks;
    while (candidate_sources != 0) {
        const int source = static_cast<int>(std::countr_zero(candidate_sources));
        candidate_sources &= candidate_sources - 1;
        const std::uint64_t source_bit = std::uint64_t{1} << source;
        const std::uint64_t next_occupied =
            (state.occupied & ~source_bit & ~captured_bit) | target_bit;
        if ((ep_diag_candidates == 0 ||
             (detail::bishop_attacks(king, next_occupied) & ep_diag_candidates) == 0) &&
            (ep_orth_candidates == 0 ||
             (detail::rook_attacks(king, next_occupied) & ep_orth_candidates) == 0)) {
            return true;
        }
    }
    return false;
}

bool safe_king_step(const NativeState& state, int /*from*/, int to, Color side) noexcept {
    return !square_attacked(state, to, opposite(side));
}

// Emits the queen, rook, bishop, and knight promotion of one push in that
// frozen order and reports whether the sink wants the walk to continue.
template <typename MoveSink>
[[nodiscard]] bool add_promotion_moves(MoveSink& moves, Square from, Square to) {
    if (!moves.emplace_back(from, to, Promotion::queen)) return false;
    if (!moves.emplace_back(from, to, Promotion::rook)) return false;
    if (!moves.emplace_back(from, to, Promotion::bishop)) return false;
    return moves.emplace_back(from, to, Promotion::knight);
}

template <bool IncludeCheckFlags, bool QuietChecksOnly, bool DropNonCheckingQuiets,
          bool StopOnFirstCheck = false>
class SearchMetadataSink {
public:
    using MetadataTag = void;

    SearchMetadataSink(const NativeState& state,
                       MoveMetadataList& output) noexcept
        : SearchMetadataSink(state, &output) {}

    explicit SearchMetadataSink(const NativeState& state) noexcept
        requires (StopOnFirstCheck)
        : SearchMetadataSink(state, nullptr) {}

    [[nodiscard]] bool found_check() const noexcept { return found_check_; }

private:
    SearchMetadataSink(const NativeState& state,
                       MoveMetadataList* output) noexcept
        : state_(state),
          output_(output),
          storage_(output != nullptr ? output->begin() : nullptr),
          count_(output != nullptr ? output->size() : 0U),
          position_key_(state.key) {
        if constexpr (!IncludeCheckFlags) {
            return;
        }
        const std::size_t side_idx = state.side == Color::white ? 0U : 1U;
        const std::size_t opp_idx = 1U - side_idx;
        enemy_king_ = state.piece_bitboards[opp_idx][static_cast<std::size_t>(PieceType::king)];
        friendly_rook_queen_ =
            state.piece_bitboards[side_idx][static_cast<std::size_t>(PieceType::rook)] |
            state.piece_bitboards[side_idx][static_cast<std::size_t>(PieceType::queen)];
        friendly_bishop_queen_ =
            state.piece_bitboards[side_idx][static_cast<std::size_t>(PieceType::bishop)] |
            state.piece_bitboards[side_idx][static_cast<std::size_t>(PieceType::queen)];
        const std::uint64_t promo_pawns =
            state.piece_bitboards[side_idx][static_cast<std::size_t>(PieceType::pawn)] &
            (state.side == Color::white ? 0x00FF000000000000ULL : 0x000000000000FF00ULL);
        if (enemy_king_ != 0 && (enemy_king_ & (enemy_king_ - 1U)) == 0U) {
            king_square_ = static_cast<int>(std::countr_zero(enemy_king_));
            rook_rays_pre_ = (friendly_rook_queen_ | promo_pawns) != 0
                ? detail::rook_attacks(king_square_, state.occupied)
                : 0ULL;
            bishop_rays_pre_ = (friendly_bishop_queen_ | promo_pawns) != 0
                ? detail::bishop_attacks(king_square_, state.occupied)
                : 0ULL;
            pre_rays_ = rook_rays_pre_ | bishop_rays_pre_;
            direct_pawn_checks_ =
                detail::pawn_attackers_of(king_square_, state.side == Color::white);
            direct_knight_checks_ = detail::knight_attacks(king_square_);
            direct_king_checks_ = detail::king_attacks(king_square_);
            const bool already_checking_sliders =
                ((rook_rays_pre_ & friendly_rook_queen_) |
                 (bishop_rays_pre_ & friendly_bishop_queen_)) != 0;
            can_fast_direct_ = !already_checking_sliders;
            if (can_fast_direct_) {
                const std::uint64_t friendly = state.occupancy[side_idx];
                const std::uint64_t rook_snipers = friendly_rook_queen_ != 0
                    ? (detail::rook_pseudo_attacks(king_square_) & friendly_rook_queen_)
                    : 0ULL;
                const std::uint64_t rook_blockers =
                    rook_snipers != 0 ? (rook_rays_pre_ & friendly) : 0ULL;
                if (rook_blockers != 0) {
                    std::uint64_t pinners =
                        detail::rook_attacks(king_square_, state.occupied ^ rook_blockers) &
                        rook_snipers;
                    while (pinners != 0) {
                        const int sniper = static_cast<int>(std::countr_zero(pinners));
                        pinners &= pinners - 1;
                        dc_blockers_ |= detail::between_mask(king_square_, sniper) & rook_blockers;
                    }
                }
                const std::uint64_t bishop_snipers = friendly_bishop_queen_ != 0
                    ? (detail::bishop_pseudo_attacks(king_square_) & friendly_bishop_queen_)
                    : 0ULL;
                const std::uint64_t bishop_blockers =
                    bishop_snipers != 0 ? (bishop_rays_pre_ & friendly) : 0ULL;
                if (bishop_blockers != 0) {
                    std::uint64_t pinners =
                        detail::bishop_attacks(king_square_, state.occupied ^ bishop_blockers) &
                        bishop_snipers;
                    while (pinners != 0) {
                        const int sniper = static_cast<int>(std::countr_zero(pinners));
                        pinners &= pinners - 1;
                        dc_blockers_ |=
                            detail::between_mask(king_square_, sniper) & bishop_blockers;
                    }
                }
            }
        }
    }

public:

    [[nodiscard]] static constexpr bool has_check_probe() noexcept { return IncludeCheckFlags; }
    [[nodiscard]] bool can_fast_direct() const noexcept { return can_fast_direct_; }
    [[nodiscard]] std::uint64_t dc_blockers() const noexcept { return dc_blockers_; }
    [[nodiscard]] int enemy_king_square() const noexcept { return king_square_; }
    [[nodiscard]] std::uint64_t direct_pawn_checks() const noexcept { return direct_pawn_checks_; }
    [[nodiscard]] std::uint64_t direct_knight_checks() const noexcept { return direct_knight_checks_; }
    [[nodiscard]] std::uint64_t rook_rays_pre() const noexcept { return rook_rays_pre_; }
    [[nodiscard]] std::uint64_t bishop_rays_pre() const noexcept { return bishop_rays_pre_; }

    [[nodiscard]] bool pre_ray_hits(const int from) const noexcept {
        if constexpr (!IncludeCheckFlags) {
            return false;
        } else {
            return ((std::uint64_t{1} << from) & pre_rays_) != 0;
        }
    }

    [[nodiscard]] bool dc_hits(const int from) const noexcept {
        if constexpr (!IncludeCheckFlags) {
            return false;
        } else {
            return ((std::uint64_t{1} << from) & dc_blockers_) != 0;
        }
    }

    [[nodiscard]] bool capture_from_safe(const int from) const noexcept {
        if constexpr (!IncludeCheckFlags || QuietChecksOnly) {
            return true;
        } else {
            return !dc_hits(from);
        }
    }

    [[nodiscard]]
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    __forceinline
#endif
    bool emit_piece_move(const Square from_sq, const int from, const int to,
                                       const PieceType moving, const PieceType captured,
                                       const MoveKind kind,
                                       const bool from_safe) noexcept {
        bool gives_check = false;
        if constexpr (IncludeCheckFlags) {
            if (!(QuietChecksOnly && kind == MoveKind::capture)) {
                if (can_fast_direct_) {
                    const std::uint64_t dest_bit = std::uint64_t{1} << to;
                    switch (moving) {
                        case PieceType::pawn:
                            gives_check = (dest_bit & direct_pawn_checks_) != 0;
                            break;
                        case PieceType::knight:
                            gives_check = (dest_bit & direct_knight_checks_) != 0;
                            break;
                        case PieceType::bishop:
                            gives_check = (dest_bit & bishop_rays_pre_) != 0;
                            break;
                        case PieceType::rook:
                            gives_check = (dest_bit & rook_rays_pre_) != 0;
                            break;
                        case PieceType::queen:
                            gives_check = (dest_bit & pre_rays_) != 0;
                            break;
                        case PieceType::king:
                            gives_check = (dest_bit & direct_king_checks_) != 0;
                            break;
                        default:
                            break;
                    }
                    if (!gives_check && !from_safe) {
                        gives_check = (detail::line_mask(king_square_, from) & dest_bit) == 0;
                    }
                } else {
                    gives_check = fallback_gives_check(from, to, moving, kind);
                }
            }
        }
        if constexpr (DropNonCheckingQuiets) {
            if (kind == MoveKind::quiet && !gives_check) {
                return true;
            }
        }
        const Square to_sq = Square::from_index(static_cast<std::uint8_t>(to));
        return append_record(Move(from_sq, to_sq), moving, captured, kind, gives_check);
    }

    [[nodiscard]]
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
    __forceinline
#endif
    bool emit_pawn_move(const Square from_sq, const int from, const int to,
                                      const bool is_promotion, const Piece target,
                                      const bool from_safe) noexcept {
        const Square to_sq = Square::from_index(static_cast<std::uint8_t>(to));
        const PieceType cap = target.empty() ? PieceType::none : target.type;
        if (!is_promotion) {
            const MoveKind kind = cap == PieceType::none ? MoveKind::quiet : MoveKind::capture;
            return emit_piece_move(from_sq, from, to, PieceType::pawn, cap, kind, from_safe);
        }
        constexpr bool check_eligible = IncludeCheckFlags && !QuietChecksOnly;
        const std::uint64_t dest_bit = std::uint64_t{1} << to;
        const bool fast = check_eligible && can_fast_direct_ && !pre_ray_hits(from);
        const auto promo_check = [&](const Promotion promo, const std::uint64_t mask,
                                     const PieceType pt) noexcept -> bool {
            const bool gives_check = check_eligible &&
                (fast ? ((dest_bit & mask) != 0)
                      : fallback_gives_check(from, to, pt, MoveKind::promotion));
            return append_record(Move(from_sq, to_sq, promo),
                                 PieceType::pawn, cap, MoveKind::promotion, gives_check);
        };
        if (!promo_check(Promotion::queen, pre_rays_, PieceType::queen)) return false;
        if (!promo_check(Promotion::rook, rook_rays_pre_, PieceType::rook)) return false;
        if (!promo_check(Promotion::bishop, bishop_rays_pre_, PieceType::bishop)) return false;
        return promo_check(Promotion::knight, direct_knight_checks_, PieceType::knight);
    }

    [[nodiscard]] bool emit_en_passant(const Square from_sq, const Square to_sq,
                                       const int from, const int to) noexcept {
        const bool gives_check = IncludeCheckFlags && !QuietChecksOnly &&
            fallback_gives_check(from, to, PieceType::pawn, MoveKind::en_passant);
        return append_record(Move(from_sq, to_sq),
                             PieceType::pawn, PieceType::pawn, MoveKind::en_passant, gives_check);
    }

    [[nodiscard]] bool emit_castling(const Square from_sq, const Square to_sq,
                                     const int from, const int to) noexcept {
        const bool gives_check = IncludeCheckFlags &&
            fallback_gives_check(from, to, PieceType::king, MoveKind::castling);
        if constexpr (DropNonCheckingQuiets) {
            if (!gives_check) {
                return true;
            }
        }
        return append_record(Move(from_sq, to_sq),
                             PieceType::king, PieceType::none, MoveKind::castling, gives_check);
    }

    void finish() noexcept {
        if constexpr (!StopOnFirstCheck) {
            output_->resize(count_);
        }
    }

private:
    [[nodiscard]] bool append_record(const Move move, const PieceType moving,
                                     const PieceType captured, const MoveKind kind,
                                     const bool gives_check) noexcept {
        if constexpr (StopOnFirstCheck) {
            if (gives_check) {
                found_check_ = true;
                return false;
            }
            return true;
        }
        if (count_ >= kMaximumLegalMoves) [[unlikely]] {
            return false;
        }
        MoveMetadata& slot = storage_[count_++];
        slot.move = move;
        slot.moving_piece = moving;
        slot.captured_piece = captured;
        slot.kind = kind;
        slot.gives_check = gives_check;
        slot.see_computed = false;
        slot.see_score = 0;
        slot.ordering_score = 0;
        slot.position_key = position_key_;
        slot.validation_token = 0;
        return true;
    }

    [[nodiscard]]
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((noinline))
#elif defined(_MSC_VER)
    __declspec(noinline)
#endif
    bool fallback_gives_check(const int from, const int to,
                                            const PieceType moved_type,
                                            const MoveKind kind) const noexcept {
        if (enemy_king_ == 0) {
            return false;
        }
        if (king_slider_rays_ == 0) {
            const int ksq = king_square_ >= 0 ? king_square_ :
                static_cast<int>(std::countr_zero(enemy_king_));
            king_slider_rays_ = detail::rook_pseudo_attacks(ksq) |
                detail::bishop_pseudo_attacks(ksq);
        }
        const std::uint64_t source_bit = std::uint64_t{1} << from;
        const std::uint64_t destination_bit = std::uint64_t{1} << to;
        std::uint64_t en_passant_victim_bit = 0;
        if (kind == MoveKind::en_passant) {
            const int captured_square = to + (state_.side == Color::white ? -8 : 8);
            if (valid_square(captured_square)) {
                en_passant_victim_bit = std::uint64_t{1} << captured_square;
            }
        }
        std::uint64_t occupied =
            (state_.occupied & ~source_bit & ~en_passant_victim_bit) | destination_bit;

        int rook_destination = -1;
        std::uint64_t castling_rook_source_bit = 0;
        if (kind == MoveKind::castling) {
            const bool king_side = to > from;
            const int rook_source = (from / 8) * 8 + (king_side ? 7 : 0);
            rook_destination = (from / 8) * 8 + (king_side ? 5 : 3);
            castling_rook_source_bit = std::uint64_t{1} << rook_source;
            occupied &= ~castling_rook_source_bit;
            occupied |= std::uint64_t{1} << rook_destination;
        }

        const bool single_king_probe = king_square_ >= 0;
        const bool rays_known = king_slider_rays_ != 0;
        const std::uint64_t real_vacated =
            source_bit | en_passant_victim_bit | castling_rook_source_bit;
        const bool maybe_discovered = !rays_known || (real_vacated & king_slider_rays_) != 0;
        const bool destination_on_ray = !rays_known || (destination_bit & king_slider_rays_) != 0;

        const int king_square = single_king_probe ?
            king_square_ : static_cast<int>(std::countr_zero(enemy_king_));
        std::uint64_t rook_rays_from_king = 0;
        std::uint64_t bishop_rays_from_king = 0;
        bool rook_rays_ready = false;
        bool bishop_rays_ready = false;
        const auto rook_rays = [&]() noexcept {
            if (!rook_rays_ready) {
                rook_rays_from_king = detail::rook_attacks(king_square, occupied);
                rook_rays_ready = true;
            }
            return rook_rays_from_king;
        };
        const auto bishop_rays = [&]() noexcept {
            if (!bishop_rays_ready) {
                bishop_rays_from_king = detail::bishop_attacks(king_square, occupied);
                bishop_rays_ready = true;
            }
            return bishop_rays_from_king;
        };

        bool direct_check = false;
        switch (moved_type) {
            case PieceType::pawn:
                direct_check = single_king_probe ?
                    (destination_bit & direct_pawn_checks_) != 0 :
                    (detail::pawn_attacks(to, state_.side == Color::white) & enemy_king_) != 0;
                break;
            case PieceType::knight:
                direct_check = single_king_probe ?
                    (destination_bit & direct_knight_checks_) != 0 :
                    (detail::knight_attacks(to) & enemy_king_) != 0;
                break;
            case PieceType::king:
                direct_check = single_king_probe ?
                    (destination_bit & direct_king_checks_) != 0 :
                    (detail::king_attacks(to) & enemy_king_) != 0;
                break;
            case PieceType::bishop:
                if (destination_on_ray &&
                    (!rays_known ||
                     (detail::bishop_pseudo_attacks(king_square) & destination_bit) != 0)) {
                    direct_check = (bishop_rays() & destination_bit) != 0;
                }
                break;
            case PieceType::rook:
                if (destination_on_ray &&
                    (!rays_known ||
                     (detail::rook_pseudo_attacks(king_square) & destination_bit) != 0)) {
                    direct_check = (rook_rays() & destination_bit) != 0;
                }
                break;
            case PieceType::queen:
                if (destination_on_ray) {
                    const bool on_rook = !rays_known ||
                        (detail::rook_pseudo_attacks(king_square) & destination_bit) != 0;
                    const bool on_bishop = !rays_known ||
                        (detail::bishop_pseudo_attacks(king_square) & destination_bit) != 0;
                    direct_check =
                        (on_rook && (rook_rays() & destination_bit) != 0) ||
                        (on_bishop && (bishop_rays() & destination_bit) != 0);
                }
                break;
            default:
                break;
        }
        if (kind == MoveKind::castling && valid_square(rook_destination)) {
            const std::uint64_t rook_dest_bit = std::uint64_t{1} << rook_destination;
            if ((detail::rook_pseudo_attacks(king_square) & rook_dest_bit) != 0 &&
                (rook_rays() & rook_dest_bit) != 0) {
                direct_check = true;
            }
        }
        if (direct_check) {
            return true;
        }
        if (maybe_discovered) {
            std::uint64_t rq = friendly_rook_queen_ & ~source_bit;
            std::uint64_t bq = friendly_bishop_queen_ & ~source_bit;
            if (kind == MoveKind::castling) {
                rq &= ~castling_rook_source_bit;
                bq &= ~castling_rook_source_bit;
            }
            const std::uint64_t rq_cand = rq != 0
                ? (detail::rook_pseudo_attacks(king_square) & rq)
                : 0ULL;
            const std::uint64_t bq_cand = bq != 0
                ? (detail::bishop_pseudo_attacks(king_square) & bq)
                : 0ULL;
            if ((rq_cand != 0 && (rook_rays() & rq_cand) != 0) ||
                (bq_cand != 0 && (bishop_rays() & bq_cand) != 0)) {
                return true;
            }
        }
        return false;
    }

    const NativeState& state_;
    MoveMetadataList* output_ = nullptr;
    MoveMetadata* storage_ = nullptr;
    std::size_t count_ = 0;
    std::uint64_t position_key_ = 0;
    std::uint64_t enemy_king_ = 0;
    std::uint64_t friendly_rook_queen_ = 0;
    std::uint64_t friendly_bishop_queen_ = 0;
    mutable std::uint64_t king_slider_rays_ = 0;
    std::uint64_t rook_rays_pre_ = 0;
    std::uint64_t bishop_rays_pre_ = 0;
    std::uint64_t pre_rays_ = 0;
    std::uint64_t dc_blockers_ = 0;
    std::uint64_t direct_pawn_checks_ = 0;
    std::uint64_t direct_knight_checks_ = 0;
    std::uint64_t direct_king_checks_ = 0;
    int king_square_ = -1;
    bool can_fast_direct_ = false;
    bool found_check_ = false;
};

template <typename T>
inline constexpr bool is_metadata_sink_v = requires { typename T::MetadataTag; };

class NativePosition {
public:
    NativeState state{};

    NativePosition() { reset_startpos(); }

    bool set_fen(std::string_view fen, bool strict) noexcept {
        std::array<std::string_view, 6> fields{};
        if (!split_fields(fen, fields)) return false;

        NativeState candidate;
        candidate.board.fill({});
        int rank = 7;
        int file = 0;
        int white_kings = 0;
        int black_kings = 0;
        int white_pawns = 0;
        int black_pawns = 0;
        int white_pieces = 0;
        int black_pieces = 0;
        for (const char value : fields[0]) {
            if (value == '/') {
                if (file != 8 || rank == 0) return false;
                --rank;
                file = 0;
                continue;
            }
            if (value >= '1' && value <= '8') {
                file += value - '0';
                if (file > 8) return false;
                continue;
            }
            if (!std::string_view("PNBRQKpnbrqk").contains(value) || file >= 8 || rank < 0) {
                return false;
            }
            if ((value == 'P' || value == 'p') && (rank == 0 || rank == 7)) return false;
            const int square = rank * 8 + file++;
            candidate.board[static_cast<std::size_t>(square)] = piece_from_char(value);
            if (value == 'K') ++white_kings;
            if (value == 'k') ++black_kings;
            if (value >= 'A' && value <= 'Z') {
                ++white_pieces;
                if (value == 'P') ++white_pawns;
            } else {
                ++black_pieces;
                if (value == 'p') ++black_pawns;
            }
        }
        if (rank != 0 || file != 8 || white_kings != 1 || black_kings != 1) {
            return false;
        }
        if (strict && (white_pawns > 8 || black_pawns > 8 ||
                       white_pieces > 16 || black_pieces > 16)) {
            return false;
        }
        if (fields[1] == "w") candidate.side = Color::white;
        else if (fields[1] == "b") candidate.side = Color::black;
        else return false;

        candidate.castling = 0;
        if (fields[2] != "-") {
            if (fields[2].empty() || fields[2].size() > 4) return false;
            for (std::size_t index = 0; index < fields[2].size(); ++index) {
                const char right = fields[2][index];
                if (!std::string_view("KQkq").contains(right) ||
                    fields[2].find(right, index + 1) != std::string_view::npos) return false;
                if (right == 'K') candidate.castling |= kWhiteKingSide;
                if (right == 'Q') candidate.castling |= kWhiteQueenSide;
                if (right == 'k') candidate.castling |= kBlackKingSide;
                if (right == 'q') candidate.castling |= kBlackQueenSide;
            }
        }
        if ((candidate.castling & kWhiteKingSide) != 0 &&
            (candidate.board[4].type != PieceType::king || candidate.board[4].color != Color::white ||
             candidate.board[7].type != PieceType::rook || candidate.board[7].color != Color::white)) return false;
        if ((candidate.castling & kWhiteQueenSide) != 0 &&
            (candidate.board[4].type != PieceType::king || candidate.board[4].color != Color::white ||
             candidate.board[0].type != PieceType::rook || candidate.board[0].color != Color::white)) return false;
        if ((candidate.castling & kBlackKingSide) != 0 &&
            (candidate.board[60].type != PieceType::king || candidate.board[60].color != Color::black ||
             candidate.board[63].type != PieceType::rook || candidate.board[63].color != Color::black)) return false;
        if ((candidate.castling & kBlackQueenSide) != 0 &&
            (candidate.board[60].type != PieceType::king || candidate.board[60].color != Color::black ||
             candidate.board[56].type != PieceType::rook || candidate.board[56].color != Color::black)) return false;

        std::uint32_t halfmove = 0;
        std::uint32_t fullmove = 0;
        if (!parse_uint(fields[4], halfmove, true, 255) || !parse_uint(fields[5], fullmove, false, 32768)) return false;
        candidate.halfmove = static_cast<std::uint16_t>(halfmove);
        candidate.fullmove = static_cast<std::uint16_t>(fullmove);
        if (fields[3] != "-") {
            if (candidate.halfmove != 0) return false;
            const auto parsed = Square::parse(fields[3]);
            if (!parsed.has_value()) return false;
            const int target = parsed->index();
            if ((candidate.side == Color::white && rank_of(target) != 5) ||
                (candidate.side == Color::black && rank_of(target) != 2) ||
                !candidate.board[static_cast<std::size_t>(target)].empty()) return false;
            const int behind = target + (candidate.side == Color::white ? -8 : 8);
            const int origin = target + (candidate.side == Color::white ? 8 : -8);
            const Piece expected{PieceType::pawn, opposite(candidate.side)};
            if (!valid_square(behind) || !valid_square(origin) ||
                candidate.board[static_cast<std::size_t>(behind)].type != expected.type ||
                candidate.board[static_cast<std::size_t>(behind)].color != expected.color ||
                !candidate.board[static_cast<std::size_t>(origin)].empty()) return false;
            candidate.en_passant = *parsed;
        }

        rebuild_derived(candidate);
        // Canonicalize the en-passant field exactly as the make path does: a
        // target square that no legal capture can use is not part of the
        // position.  The position key and the shadow board already ignore it,
        // so keeping the raw FEN text here made native/shadow look inconsistent
        // and the completion gate then discarded a fully searched move.
        if (candidate.en_passant.index() < Square::kInvalid &&
            !has_legal_en_passant_capture(candidate)) {
            candidate.en_passant = {};
        }
        const int white_king = king_square(candidate, Color::white);
        const int black_king = king_square(candidate, Color::black);
        if (strict) {
            if (std::abs(file_of(white_king) - file_of(black_king)) <= 1 &&
                std::abs(rank_of(white_king) - rank_of(black_king)) <= 1) return false;
            if (attacker_count(candidate, white_king, Color::black) > 2 ||
                attacker_count(candidate, black_king, Color::white) > 2) return false;
            if (is_checked(candidate, Color::white) && is_checked(candidate, Color::black)) return false;
            // Positions whose non-moving side is already in check stay
            // accepted: they are unreachable in a real game, but composed
            // analysis boards and the project's own tactical fixtures use
            // them, and the search treats them as ordinary positions.
        }
        candidate.history_size = 0;
        state = candidate;
        return true;
    }

    void reset_startpos() noexcept {
        (void)set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", true);
    }

    std::string fen() const {
        std::string result;
        for (int rank = 7; rank >= 0; --rank) {
            int empty = 0;
            for (int file = 0; file < 8; ++file) {
                const Piece piece = state.board[static_cast<std::size_t>(rank * 8 + file)];
                if (piece.empty()) {
                    ++empty;
                } else {
                    if (empty != 0) result += static_cast<char>('0' + empty);
                    empty = 0;
                    result += piece_to_char(piece);
                }
            }
            if (empty != 0) result += static_cast<char>('0' + empty);
            if (rank != 0) result += '/';
        }
        result += state.side == Color::white ? " w " : " b ";
        if (state.castling == 0) result += '-';
        else {
            if (state.castling & kWhiteKingSide) result += 'K';
            if (state.castling & kWhiteQueenSide) result += 'Q';
            if (state.castling & kBlackKingSide) result += 'k';
            if (state.castling & kBlackQueenSide) result += 'q';
        }
        result += ' ';
        result += state.en_passant.index() < Square::kInvalid ? state.en_passant.uci() : "-";
        result += ' ' + std::to_string(state.halfmove);
        result += ' ' + std::to_string(state.fullmove);
        return result;
    }

    [[nodiscard]] Color side_to_move() const noexcept { return state.side; }

    [[nodiscard]] Piece piece_at(Square square) const noexcept {
        if (square.index() >= Square::kInvalid) {
            return {};
        }
        return state.board[square.index()];
    }

    // King moves are decided directly: after the king vacates `from`, the
    // destination must not be attacked.  Castling is already fully validated
    // during generation (path clear, not in check, transit and destination
    // safe), so it needs no probe at all.  Clearing `from` from the occupancy
    // is what makes the ray the king steps along visible; without it a
    // checking slider behind the king would be missed.
    [[nodiscard]] bool king_move_is_legal(const Move& move) noexcept {
        const int from = move.from().index();
        const int to = move.to().index();
        if (!valid_square(from) || !valid_square(to)) {
            return false;
        }
        const int from_file = file_of(from);
        const int to_file = file_of(to);
        if (from_file - to_file == 2 || to_file - from_file == 2) {
            return true;
        }
        const std::uint64_t saved_occupied = state.occupied;
        state.occupied &= ~bit(from);
        const bool attacked = square_attacked(state, to, opposite(state.side));
        state.occupied = saved_occupied;
        return !attacked;
    }

    // Full legality resolution for moves needing explicit verification
    // (king moves via king_move_is_legal; non-king, non-en-passant moves via
    // O(1) pin-line / single-check evasion masks; en passant via minimal
    // in-place trial).
    [[nodiscard]] bool resolve_move_legality(const Move& move, Color mover) {
        const int from = move.from().index();
        const int to = move.to().index();
        if (!valid_square(from) || !valid_square(to)) {
            return false;
        }
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        if (moving.type == PieceType::king) {
            return king_move_is_legal(move);
        }
        const Piece captured = state.board[static_cast<std::size_t>(to)];
        const bool en_passant = moving.type == PieceType::pawn &&
            state.en_passant.index() < Square::kInvalid &&
            to == state.en_passant.index() && captured.empty();
        const std::size_t side_index = mover == Color::white ? 0U : 1U;
        const std::uint64_t checkers = state.checkers[side_index];
        const std::uint64_t pinned = state.pinned[side_index];
        if (!en_passant) {
            const int king = king_square(state, mover);
            if (king < 0) {
                return true;
            }
            const std::uint64_t from_bit = bit(from);
            const std::uint64_t to_bit = bit(to);
            if (checkers == 0) {
                return (pinned & from_bit) == 0 ||
                    (detail::line_mask(king, from) & to_bit) != 0;
            }
            if ((checkers & (checkers - 1)) != 0 || (pinned & from_bit) != 0) {
                return false;
            }
            const int checker = static_cast<int>(std::countr_zero(checkers));
            return ((checkers | detail::between_mask(king, checker)) & to_bit) != 0;
        }

        if (checkers != 0) {
            if ((checkers & (checkers - 1)) != 0) {
                return false;
            }
            const int checker = static_cast<int>(std::countr_zero(checkers));
            const int captured_square = mover == Color::white ? to - 8 : to + 8;
            if ((bit(to) & checkers) == 0 && captured_square != checker) {
                const int king = king_square(state, mover);
                if (king < 0 || (detail::between_mask(king, checker) & bit(to)) == 0) {
                    return false;
                }
            }
        }

        const int victim_square = to + (moving.color == Color::white ? -8 : 8);
        const Piece victim = state.board[static_cast<std::size_t>(victim_square)];
        const std::size_t opponent_index = 1U - side_index;
        const std::size_t victim_type = static_cast<std::size_t>(victim.type);
        const std::uint64_t saved_victim_board =
            state.piece_bitboards[opponent_index][victim_type];
        const std::uint64_t saved_occupied = state.occupied;
        state.piece_bitboards[opponent_index][victim_type] &= ~bit(victim_square);
        state.occupied =
            (saved_occupied & ~bit(from) & ~bit(victim_square)) | bit(to);
        const bool legal = !is_checked(state, mover);
        state.occupied = saved_occupied;
        state.piece_bitboards[opponent_index][victim_type] = saved_victim_board;
        return legal;
    }

    // Sink for the existence probe: every emitted candidate from
    // generate_pseudo() is already legal, so the first emission stops the walk.
    class FirstLegalMoveSink {
    public:
        template <typename... Arguments>
        [[nodiscard]] bool emplace_back(Arguments&&...) noexcept {
            found_ = true;
            return false;
        }

        [[nodiscard]] bool found() const noexcept { return found_; }

    private:
        bool found_ = false;
    };

    std::vector<Move> legal_moves() {
        std::vector<Move> result;
        result.reserve(64);
        VectorMoveSink sink(result);
        generate_pseudo(sink);
        return result;
    }

    enum class PseudoGenMode : std::uint8_t { all, tactical_only, tactical_and_checks, quiet_only };

    template <PseudoGenMode Mode>
    std::size_t legal_moves_into_impl(std::span<Move> output) {
        SpanMoveSink sink(output);
        generate_pseudo<Mode>(sink);
        return sink.count();
    }

    std::size_t legal_moves_into(std::span<Move> output) {
        return legal_moves_into_impl<PseudoGenMode::all>(output);
    }

    [[nodiscard]] std::size_t count_legal_moves_up_to(const std::size_t limit) noexcept {
        if (limit == 0) {
            return 0;
        }
        BoundedCountMoveSink sink(limit);
        generate_pseudo<PseudoGenMode::all>(sink);
        return sink.count();
    }

    [[nodiscard]] bool has_legal_checking_move() noexcept {
        SearchMetadataSink<true, false, true, true> sink(state);
        generate_pseudo<PseudoGenMode::tactical_and_checks>(sink);
        return sink.found_check();
    }

    std::size_t legal_tactical_moves_into(std::span<Move> output) {
        return legal_moves_into_impl<PseudoGenMode::tactical_only>(output);
    }

    std::size_t legal_tactical_and_checking_moves_into(std::span<Move> output) {
        return legal_moves_into_impl<PseudoGenMode::tactical_and_checks>(output);
    }

    void legal_search_metadata_into(MoveMetadataList& output,
                                    const bool include_check_flags,
                                    const bool quiet_checks_only) noexcept {
        output.clear();
        if (include_check_flags) {
            if (quiet_checks_only) {
                SearchMetadataSink<true, true, false> sink(state, output);
                generate_pseudo<PseudoGenMode::all>(sink);
                sink.finish();
            } else {
                SearchMetadataSink<true, false, false> sink(state, output);
                generate_pseudo<PseudoGenMode::all>(sink);
                sink.finish();
            }
        } else {
            SearchMetadataSink<false, false, false> sink(state, output);
            generate_pseudo<PseudoGenMode::all>(sink);
            sink.finish();
        }
    }

    [[nodiscard]] bool legal_tactical_search_metadata_into(
        MoveMetadataList& output, const bool include_quiet_checks,
        const bool known_has_legal_move = false) noexcept {
        output.clear();
        const bool checked = state.checkers[state.side == Color::white ? 0U : 1U] != 0;
        if (checked) {
            SearchMetadataSink<true, false, false> sink(state, output);
            generate_pseudo<PseudoGenMode::all>(sink);
            sink.finish();
            return !output.empty();
        }
        if (include_quiet_checks) {
            SearchMetadataSink<true, false, true> sink(state, output);
            generate_pseudo<PseudoGenMode::tactical_and_checks>(sink);
            sink.finish();
        } else {
            SearchMetadataSink<true, false, false> sink(state, output);
            generate_pseudo<PseudoGenMode::tactical_only>(sink);
            sink.finish();
        }
        return known_has_legal_move || !output.empty() || has_legal_move();
    }

    void legal_tactical_only_search_metadata_into(MoveMetadataList& output) noexcept {
        output.clear();
        SearchMetadataSink<false, false, false> sink(state, output);
        generate_pseudo<PseudoGenMode::tactical_only>(sink);
        sink.finish();
    }

    void append_quiet_search_metadata_into(MoveMetadataList& output) noexcept {
        SearchMetadataSink<true, true, false> sink(state, output);
        generate_pseudo<PseudoGenMode::quiet_only>(sink);
        sink.finish();
    }

    [[nodiscard]] bool has_quick_unpinned_legal_move() const noexcept {
        const Color side = state.side;
        const std::size_t side_index = side == Color::white ? 0U : 1U;
        const std::size_t enemy_index = 1U - side_index;
        if (state.checkers[side_index] != 0 || king_square(state, side) < 0) {
            return false;
        }
        constexpr std::uint64_t kFileAMask = 0x0101010101010101ULL;
        constexpr std::uint64_t kFileHMask = 0x8080808080808080ULL;
        const std::uint64_t pinned = state.pinned[side_index];
        const std::uint64_t friendly = state.occupancy[side_index];
        const std::uint64_t enemy_king_bb =
            state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::king)];
        const std::uint64_t valid_targets = ~(friendly | enemy_king_bb);

        const std::uint64_t unpinned_pawns =
            state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::pawn)] & ~pinned;
        const std::uint64_t empty = ~state.occupied;
        const std::uint64_t single_push = side == Color::white
            ? ((unpinned_pawns << 8) & empty)
            : ((unpinned_pawns >> 8) & empty);
        if (single_push != 0) {
            return true;
        }
        const std::uint64_t enemy_occupancy = state.occupancy[enemy_index] & ~enemy_king_bb;
        const std::uint64_t pawn_captures = side == Color::white
            ? ((((unpinned_pawns & ~kFileAMask) << 7) |
                ((unpinned_pawns & ~kFileHMask) << 9)) & enemy_occupancy)
            : ((((unpinned_pawns & ~kFileHMask) >> 7) |
                ((unpinned_pawns & ~kFileAMask) >> 9)) & enemy_occupancy);
        if (pawn_captures != 0) {
            return true;
        }

        const std::uint64_t unpinned_ortho =
            (state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::rook)] |
             state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::queen)]) & ~pinned;
        if ((((unpinned_ortho << 8) | (unpinned_ortho >> 8) |
              ((unpinned_ortho & ~kFileHMask) << 1) |
              ((unpinned_ortho & ~kFileAMask) >> 1)) & valid_targets) != 0) {
            return true;
        }

        const std::uint64_t unpinned_diag =
            (state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::bishop)] |
             state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::queen)]) & ~pinned;
        return ((((unpinned_diag & ~kFileHMask) << 9) |
                 ((unpinned_diag & ~kFileAMask) << 7) |
                 ((unpinned_diag & ~kFileHMask) >> 7) |
                 ((unpinned_diag & ~kFileAMask) >> 9)) & valid_targets) != 0;
    }

    // Stops at the first legal candidate instead of materializing the whole
    // list.  Quiescence only needs to know whether a legal reply exists before
    // deciding that a position is terminal.
    [[nodiscard]] bool has_legal_move() {
        const Color side = state.side;
        const std::size_t side_index = side == Color::white ? 0U : 1U;
        const std::size_t enemy_index = 1U - side_index;
        if (state.checkers[side_index] == 0 && king_square(state, side) >= 0) {
            if (has_quick_unpinned_legal_move()) {
                return true;
            }
            const std::uint64_t pinned = state.pinned[side_index];
            const std::uint64_t friendly = state.occupancy[side_index];
            const std::uint64_t enemy_king_bb =
                state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::king)];
            const std::uint64_t valid_targets = ~(friendly | enemy_king_bb);

            std::uint64_t unpinned_knights =
                state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::knight)] & ~pinned;
            while (unpinned_knights != 0) {
                const int from = static_cast<int>(std::countr_zero(unpinned_knights));
                if ((detail::knight_attacks(from) & valid_targets) != 0) {
                    return true;
                }
                unpinned_knights &= unpinned_knights - 1;
            }
        }
        FirstLegalMoveSink sink;
        generate_pseudo(sink);
        return sink.found();
    }

    template <bool FillFacts>
    [[nodiscard]] bool is_legal_impl(const Move& move,
                                     Position::MoveFacts* facts) const noexcept {
        if (move.is_no_move()) {
            return false;
        }
        const int from = move.from().index();
        const int to = move.to().index();
        if (!valid_square(from) || !valid_square(to) || from == to) {
            return false;
        }
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        if (moving.empty() || moving.color != state.side) {
            return false;
        }
        const Piece target = state.board[static_cast<std::size_t>(to)];
        if (!target.empty() && (target.color == state.side || target.type == PieceType::king)) {
            return false;
        }
        if (!is_legal_after_endpoints(move, from, to, moving, target)) {
            return false;
        }
        if constexpr (FillFacts) {
            facts->valid = true;
            facts->occupied_from = true;
            facts->occupied_to = !target.empty();
            facts->own_piece_on_from = true;
            facts->en_passant_target = to == state.en_passant.index();
            facts->from = moving;
            facts->to = target;
        }
        return true;
    }

    [[nodiscard]] bool is_legal(const Move& move) const noexcept {
        return is_legal_impl<false>(move, nullptr);
    }

    [[nodiscard]] bool try_legal_move_facts(const Move& move,
                                            Position::MoveFacts& facts) const noexcept {
        return is_legal_impl<true>(move, &facts);
    }

    [[nodiscard]] bool is_legal_after_endpoints(const Move& move, const int from, const int to,
                                                const Piece moving,
                                                const Piece target) const noexcept {
        auto* mut = const_cast<NativePosition*>(this);
        if (moving.type == PieceType::pawn) {
            const int promo_rank = state.side == Color::white ? 7 : 0;
            if ((rank_of(to) == promo_rank) != (move.promotion() != Promotion::none)) {
                return false;
            }
            const int dir = state.side == Color::white ? 1 : -1;
            const int rank_diff = rank_of(to) - rank_of(from);
            const int file_diff = std::abs(file_of(to) - file_of(from));
            if (file_diff == 0) {
                if (!target.empty()) {
                    return false;
                }
                if (rank_diff == 2 * dir) {
                    const int start_rank = state.side == Color::white ? 1 : 6;
                    if (rank_of(from) != start_rank ||
                        !state.board[static_cast<std::size_t>(from + dir * 8)].empty()) {
                        return false;
                    }
                } else if (rank_diff != dir) {
                    return false;
                }
            } else if (file_diff == 1 && rank_diff == dir) {
                if (target.empty() && to != state.en_passant.index()) {
                    return false;
                }
            } else {
                return false;
            }
            return mut->resolve_move_legality(move, state.side);
        }
        if (move.promotion() != Promotion::none) {
            return false;
        }
        switch (moving.type) {
            case PieceType::knight:
                return (detail::knight_attacks(from) & bit(to)) != 0 &&
                    mut->resolve_move_legality(move, state.side);
            case PieceType::bishop:
                return (detail::bishop_pseudo_attacks(from) & bit(to)) != 0 &&
                    (detail::bishop_attacks(from, state.occupied) & bit(to)) != 0 &&
                    mut->resolve_move_legality(move, state.side);
            case PieceType::rook:
                return (detail::rook_pseudo_attacks(from) & bit(to)) != 0 &&
                    (detail::rook_attacks(from, state.occupied) & bit(to)) != 0 &&
                    mut->resolve_move_legality(move, state.side);
            case PieceType::queen: {
                const std::uint64_t to_bit = bit(to);
                const bool reaches =
                    ((detail::bishop_pseudo_attacks(from) & to_bit) != 0 &&
                     (detail::bishop_attacks(from, state.occupied) & to_bit) != 0) ||
                    ((detail::rook_pseudo_attacks(from) & to_bit) != 0 &&
                     (detail::rook_attacks(from, state.occupied) & to_bit) != 0);
                return reaches && mut->resolve_move_legality(move, state.side);
            }
            case PieceType::king: {
                if ((detail::king_attacks(from) & bit(to)) != 0) {
                    return mut->king_move_is_legal(move);
                }
                const int rank_base = state.side == Color::white ? 0 : 7;
                const std::size_t side_index = state.side == Color::white ? 0U : 1U;
                if (from == rank_base * 8 + 4 && rank_of(to) == rank_base &&
                    king_square(state, state.side) >= 0 && state.checkers[side_index] == 0) {
                    if (to == rank_base * 8 + 6) {
                        const std::uint8_t right =
                            state.side == Color::white ? kWhiteKingSide : kBlackKingSide;
                        return (state.castling & right) != 0 &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 5)].empty() &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 6)].empty() &&
                            safe_king_step(mut->state, from, rank_base * 8 + 5, state.side) &&
                            safe_king_step(mut->state, from, rank_base * 8 + 6, state.side);
                    }
                    if (to == rank_base * 8 + 2) {
                        const std::uint8_t right =
                            state.side == Color::white ? kWhiteQueenSide : kBlackQueenSide;
                        return (state.castling & right) != 0 &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 1)].empty() &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 2)].empty() &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 3)].empty() &&
                            safe_king_step(mut->state, from, rank_base * 8 + 3, state.side) &&
                            safe_king_step(mut->state, from, rank_base * 8 + 2, state.side);
                    }
                }
                return false;
            }
            default:
                return false;
        }
    }

    [[nodiscard]] bool is_capture(const Move& move) const noexcept {
        if (move.from().index() >= Square::kInvalid || move.to().index() >= Square::kInvalid) {
            return false;
        }
        const Piece moving = state.board[move.from().index()];
        if (moving.empty() || moving.color != state.side) {
            return false;
        }
        const Piece target = state.board[move.to().index()];
        return (!target.empty() && target.color != moving.color) ||
            (moving.type == PieceType::pawn && target.empty() &&
             move.to() == state.en_passant);
    }

    // Single-pass resolution of every board fact the metadata builders need.
    // The verdicts are exactly those of the individual accessors; callers that
    // need several of them for one move pay for one pass instead of six.
    [[nodiscard]] Position::MoveFacts move_facts(const Move& move) const noexcept {
        Position::MoveFacts facts;
        const std::uint8_t source = move.from().index();
        const std::uint8_t destination = move.to().index();
        if (source >= Square::kInvalid || destination >= Square::kInvalid) {
            return facts;
        }
        facts.valid = true;
        facts.from = state.board[source];
        facts.to = state.board[destination];
        facts.occupied_from = !facts.from.empty();
        facts.occupied_to = !facts.to.empty();
        facts.own_piece_on_from = facts.occupied_from && facts.from.color == state.side;
        // state.en_passant is kInvalid when no en-passant capture is
        // available, which no on-board destination can match.
        facts.en_passant_target = destination == state.en_passant.index();
        return facts;
    }

    bool apply_legal(const Move& move) {
        if (!is_legal(move)) return false;
        record_and_apply_unchecked(move);
        refresh_king_masks(state);
        return true;
    }

    bool make_move(const Move& move) noexcept {
        try {
            return apply_legal(move);
        } catch (...) {
            return false;
        }
    }

    bool make_generated_move(const Move& move) noexcept {
        if (move.from().index() >= Square::kInvalid || move.to().index() >= Square::kInvalid) {
            return false;
        }
        const Piece moving = state.board[move.from().index()];
        if (moving.empty() || moving.color != state.side) {
            return false;
        }
        record_and_apply_unchecked(move);
        refresh_king_masks(state);
        return true;
    }

    bool make_generated_move_if_key(const Move& move, const std::uint64_t expected_key) noexcept {
        if (state.key != expected_key) {
            return false;
        }
        return make_generated_move(move);
    }

    bool make_generated_move_if_key(const Move& move, const std::uint64_t expected_key,
                                    Position::KeyAndSide& child_out) noexcept {
        if (!make_generated_move_if_key(move, expected_key)) {
            return false;
        }
        child_out = position_key_and_side();
        return true;
    }

    [[nodiscard]] Position::KeyAndSide position_key_and_side() const noexcept {
        return Position::KeyAndSide{
            state.key,
            state.side,
            state.checkers[state.side == Color::white ? 0U : 1U] != 0};
    }

    bool unapply() noexcept {
        if (state.history_size == 0) return false;
        // Consume the top record and invert the make in place.  The piece
        // moves mirror apply_unchecked()'s bitboard/occupancy updates without
        // paying for Zobrist lookups since `state.key = record.key` below
        // restores the exact parent hash verbatim.
        const UndoRecord& record = state.history[--state.history_size];
        if (!record.null_move) {
            const Color mover = opposite(state.side);
            const int from = record.move.from().index();
            const int to = record.move.to().index();
            const Piece moved_now = state.board[static_cast<std::size_t>(to)];
            const Piece mover_piece = record.move.promotion() != Promotion::none
                ? Piece{PieceType::pawn, mover} : moved_now;
            if (!moved_now.empty()) {
                remove_bitboard_piece_known(state, moved_now, to);
            }
            state.board[static_cast<std::size_t>(to)] = {};
            if (!record.captured.empty()) {
                const int victim = (record.flags & kEpFlag) != 0
                    ? static_cast<int>(record.ep_victim) : to;
                state.board[static_cast<std::size_t>(victim)] = record.captured;
                add_bitboard_piece_known(state, record.captured, victim);
            }
            state.board[static_cast<std::size_t>(from)] = mover_piece;
            if (!mover_piece.empty()) {
                add_bitboard_piece_known(state, mover_piece, from);
            }
            if ((record.flags & kCastlingFlag) != 0) {
                const int rook_from = to > from ? from + 3 : from - 4;
                const int rook_to = to > from ? from + 1 : from - 1;
                const Piece rook = state.board[static_cast<std::size_t>(rook_to)];
                if (!rook.empty()) {
                    remove_bitboard_piece_known(state, rook, rook_to);
                    add_bitboard_piece_known(state, rook, rook_from);
                }
                state.board[static_cast<std::size_t>(rook_from)] = rook;
                state.board[static_cast<std::size_t>(rook_to)] = {};
            }
        }
        state.side = opposite(state.side);
        state.castling = record.prior_castling;
        state.en_passant = Square::from_index(record.prior_en_passant);
        state.halfmove = record.prior_halfmove;
        state.fullmove = record.prior_fullmove;
        state.key = record.key;
        state.repetition_history_fingerprint = record.fingerprint;
        state.repetition_history_suppressed = record.prior_suppressed;
        state.checkers = record.checkers;
        state.pinned = record.pinned;
        // `occupied == occupancy[0] | occupancy[1]` holds at every record
        // point (rebuild_derived and apply_unchecked both re-derive the
        // union), so unmake recomputes it instead of recording a copy.
        state.occupied = state.occupancy[0] | state.occupancy[1];
        return true;
    }

    bool unmake_move() noexcept {
        if (state.history_size == 0 || state.history[state.history_size - 1].null_move) {
            return false;
        }
        return unapply();
    }

    bool make_null_move() noexcept {
        record_undo_null(state);
        // A null pass touches no piece, so the board, the piece bitboards and
        // the occupancy words are already current: only the rule-state key
        // terms change.  This mirrors calculate_key(): the en-passant term is
        // present iff the predicate below held when the key was last built,
        // so probe it while the square is still set and the side to move is
        // still the side that could capture; the side-to-move term toggles
        // exactly once for the flip below.
        const ZobristKeys& keys = zobrist();
        if (has_legal_en_passant_capture(state)) {
            state.key ^= keys.en_passant[state.en_passant.index() & 7];
        }
        state.key ^= keys.black_to_move;
        state.en_passant = {};
        // Do not advance the halfmove clock: the compatibility mirror's
        // makeNullMove() leaves its clock untouched (chess-library semantics),
        // and every tracked null move would otherwise be reported inconsistent.
        // The 50/75-move claims are already suppressed for null branches.
        if (state.side == Color::black) {
            state.fullmove = static_cast<std::uint16_t>(
                std::min<unsigned>(std::numeric_limits<std::uint16_t>::max(), state.fullmove + 1));
        }
        state.side = opposite(state.side);
        // Null transitions are search-only passes. Keep the real reversible
        // history fingerprint intact, but suppress all positions derived from
        // the pass while that speculative branch remains active.
        state.repetition_history_suppressed = true;
        // Same post-null-move arm as rebuild_derived(): the new side to move
        // needs fresh checkers/pinned masks, and nothing else it rebuilt
        // (board-derived bitboards) can have changed.
        refresh_king_masks(state);
        return true;
    }

    bool make_null_move(std::uint64_t& child_key) noexcept {
        if (!make_null_move()) {
            return false;
        }
        child_key = state.key;
        return true;
    }

    bool unmake_null_move() noexcept {
        if (state.history_size == 0 || !state.history[state.history_size - 1].null_move) {
            return false;
        }
        return unapply();
    }

    [[nodiscard]] std::uint64_t position_key() const noexcept { return state.key; }

    [[nodiscard]] std::uint64_t repetition_history_fingerprint() const noexcept {
        return state.repetition_history_fingerprint;
    }

    [[nodiscard]] bool repetition_history_suppressed() const noexcept {
        return state.repetition_history_suppressed;
    }

    [[nodiscard]] std::uint64_t pawn_key() const noexcept {
        constexpr std::size_t pawn = static_cast<std::size_t>(PieceType::pawn);
        // The native state maintains these bitboards incrementally. Mix the
        // two colors before the history table masks the result so nearby
        // board squares do not create a biased bucket distribution.
        std::uint64_t seed = state.piece_bitboards[0][pawn] ^
            state.piece_bitboards[1][pawn] * 0xD6E8FEB86659FD93ULL;
        return splitmix64(seed);
    }

    [[nodiscard]] std::size_t piece_count() const noexcept {
        return static_cast<std::size_t>(std::popcount(state.occupied));
    }

    [[nodiscard]] std::uint64_t piece_bitboard(PieceType type, Color color) const noexcept {
        const std::size_t type_index = static_cast<std::size_t>(type);
        if (type_index >= state.piece_bitboards[0].size()) {
            return 0;
        }
        return state.piece_bitboards[color == Color::white ? 0 : 1][type_index];
    }

    [[nodiscard]] std::uint64_t occupied_squares() const noexcept { return state.occupied; }

    [[nodiscard]] Position::ExchangeSnapshot exchange_snapshot() const noexcept {
        return Position::ExchangeSnapshot{&state.piece_bitboards, state.occupied, state.side};
    }

    [[nodiscard]] bool in_check() const noexcept {
        // Bit-identity with is_checked(state, state.side): refresh_king_masks()
        // fills checkers[side] from exactly square_attacked()'s five terms, and
        // every public mutation refreshes the side-to-move mask (rebuild_derived
        // on set_fen and null transitions, apply_legal/make_generated_move after
        // the side flip, unapply from the undo record).  Board-mutating trial helpers
        // Trial helpers (resolve_move_legality, safe_king_step) call
        // is_checked()/square_attacked() directly and never read this mask.
        // is_checked() reports true for an absent king, so keep
        // that guard; set_fen requires one king per side, making it defensive.
        if (king_square(state, state.side) < 0) return true;
        return state.checkers[state.side == Color::white ? 0U : 1U] != 0;
    }

    [[nodiscard]] bool in_check(Color color) const noexcept { return is_checked(state, color); }

    [[nodiscard]] std::uint64_t checkers() const noexcept {
        return king_square(state, state.side) < 0
            ? 0ULL
            : state.checkers[state.side == Color::white ? 0U : 1U];
    }

    [[nodiscard]] bool has_non_pawn_material(Color color) const noexcept {
        const std::size_t color_index = color == Color::white ? 0U : 1U;
        constexpr std::size_t knight = static_cast<std::size_t>(PieceType::knight);
        constexpr std::size_t bishop = static_cast<std::size_t>(PieceType::bishop);
        constexpr std::size_t rook = static_cast<std::size_t>(PieceType::rook);
        constexpr std::size_t queen = static_cast<std::size_t>(PieceType::queen);
        return (state.piece_bitboards[color_index][knight] |
                state.piece_bitboards[color_index][bishop] |
                state.piece_bitboards[color_index][rook] |
                state.piece_bitboards[color_index][queen]) != 0;
    }

    [[nodiscard]] std::uint8_t castling_rights() const noexcept { return state.castling; }

    [[nodiscard]] Square en_passant_square() const noexcept { return state.en_passant; }

    [[nodiscard]] std::size_t history_size() const noexcept { return state.history_size; }

    [[nodiscard]] std::size_t repetition_count() const noexcept {
        // A null move is a search-only pass, not a legal game-history entry.
        // Its descendants may still make ordinary moves before the probe is
        // undone, so checking only for the most recent null record would
        // let those artificial positions participate in repetition draws.
        if (state.repetition_history_suppressed) {
            return 1;
        }
        // The shortest possible repetition cycle is four plies, so a shorter
        // halfmove clock makes the backward scan pointless.
        if (state.halfmove < 4) {
            return 1;
        }
        // A repeated key can only occur within the last `halfmove` plies:
        // an intervening pawn move, capture, or castling-right change would
        // have altered the key irreversibly, and the first two reset the
        // clock.  Clamped clocks (>= 255) keep the unbounded scan.
        std::size_t count = 1;
        const std::size_t scan = state.halfmove < 255
            ? std::min<std::size_t>(state.history_size, state.halfmove)
            : state.history_size;
        const std::size_t floor = state.history_size - scan;
        for (std::size_t index = state.history_size; index >= floor + 2U; index -= 2U) {
            if (state.history[index - 1U].null_move ||
                state.history[index - 2U].null_move) {
                break;
            }
            if (state.history[index - 2U].key == state.key) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] bool is_checkmate() const noexcept {
        try {
            auto* mutable_this = const_cast<NativePosition*>(this);
            // has_legal_move() runs the same pseudo-generation and legality
            // filter as legal_moves(), with an early exit and no allocation.
            return in_check() && !mutable_this->has_legal_move();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool can_claim_threefold_repetition() const noexcept {
        return repetition_count() >= 3 && !is_checkmate();
    }

    [[nodiscard]] bool can_claim_fifty_move_draw() const noexcept {
        // A search null move advances the stored compatibility clock, but it
        // is not a legal game move and must not create a claimable draw by
        // itself (or while a reversible descendant is still on that branch).
        return !state.repetition_history_suppressed && state.halfmove >= 100 &&
            !is_checkmate();
    }

    [[nodiscard]] bool is_automatic_fivefold_repetition() const noexcept {
        return repetition_count() >= 5 && !is_checkmate();
    }

    [[nodiscard]] bool is_automatic_seventy_five_move_draw() const noexcept {
        return !state.repetition_history_suppressed && state.halfmove >= 150 &&
            !is_checkmate();
    }

    [[nodiscard]] bool is_insufficient_material() const noexcept {
        // Bitboard form: draw_status() reaches this on every node, and the
        // incrementally maintained piece bitboards answer it with a handful
        // of popcounts instead of a 64-square scan.
        constexpr std::size_t pawn = static_cast<std::size_t>(PieceType::pawn);
        constexpr std::size_t knight = static_cast<std::size_t>(PieceType::knight);
        constexpr std::size_t bishop = static_cast<std::size_t>(PieceType::bishop);
        constexpr std::size_t rook = static_cast<std::size_t>(PieceType::rook);
        constexpr std::size_t queen = static_cast<std::size_t>(PieceType::queen);
        constexpr std::uint64_t dark_squares = [] {
            std::uint64_t mask = 0;
            for (int square = 0; square < 64; ++square) {
                if ((((square & 7) + (square >> 3)) & 1) != 0) {
                    mask |= std::uint64_t{1} << square;
                }
            }
            return mask;
        }();
        if ((state.piece_bitboards[0][pawn] | state.piece_bitboards[1][pawn] |
             state.piece_bitboards[0][rook] | state.piece_bitboards[1][rook] |
             state.piece_bitboards[0][queen] | state.piece_bitboards[1][queen]) != 0) {
            return false;
        }
        const std::uint64_t knights =
            state.piece_bitboards[0][knight] | state.piece_bitboards[1][knight];
        const std::uint64_t bishops =
            state.piece_bitboards[0][bishop] | state.piece_bitboards[1][bishop];
        if (static_cast<int>(std::popcount(knights | bishops)) <= 1) {
            return true;
        }
        // KNN vs K cannot mate: two knights of one colour and no other
        // non-king material is a dead position.
        if (bishops == 0 && std::popcount(knights) == 2 &&
            (std::popcount(state.piece_bitboards[0][knight]) == 2 ||
             std::popcount(state.piece_bitboards[1][knight]) == 2)) {
            return true;
        }
        if (knights != 0) {
            return false;
        }
        // Only bishops remain: insufficient when every bishop stands on one
        // square color.
        return (bishops & dark_squares) == 0 || (bishops & ~dark_squares) == 0;
    }

    [[nodiscard]] bool is_dead_position() const noexcept {
        // Dead-position recognition is limited to provably mate-free material.
        // A previous hardcoded "locked pawn wall" pattern was not actually dead:
        // both bishops could leave their diagonals and a legal helpmate existed,
        // so treating it as a draw terminated live games incorrectly.
        return is_insufficient_material();
    }

    // Overload taking a precomputed repetition_count() so one history scan can
    // serve both the draw status and is_repetition_sensitive() for the same
    // unchanged position.  The branches, ordering, and verdicts are identical
    // to the scanning overload below; is_checkmate() stays lazily queried.
    [[nodiscard]] DrawStatus draw_status(std::size_t repetitions) const noexcept {
        if (is_dead_position()) return DrawStatus::dead_position;
        if (is_automatic_seventy_five_move_draw()) {
            return DrawStatus::automatic_seventy_five_move;
        }
        // The fivefold and threefold checks share the caller's history scan.
        if (repetitions >= 5 && !is_checkmate()) return DrawStatus::automatic_fivefold;
        if (can_claim_fifty_move_draw()) return DrawStatus::claimable_fifty_move;
        if (repetitions >= 3 && !is_checkmate()) return DrawStatus::claimable_threefold;
        return DrawStatus::none;
    }

    [[nodiscard]] DrawStatus draw_status() const noexcept {
        return draw_status(repetition_count());
    }

    [[nodiscard]] Position::NodeEntrySnapshot node_entry_snapshot() const noexcept {
        Position::NodeEntrySnapshot out;
        out.position_key = state.key;
        out.pawn_key = pawn_key();
        out.piece_bitboards = &state.piece_bitboards;
        out.halfmove_clock = state.halfmove;
        out.side_to_move = state.side;
        out.piece_count = static_cast<std::uint8_t>(std::popcount(state.occupied));
        out.in_check = in_check();
        out.has_quick_legal_move = !out.in_check && has_quick_unpinned_legal_move();
        out.repetition_history_suppressed = state.repetition_history_suppressed;
        out.has_non_pawn_material = has_non_pawn_material(state.side);
        out.opponent_has_non_pawn_material = has_non_pawn_material(opposite(state.side));
        out.repetition_count = repetition_count();
        out.draw_status = draw_status(out.repetition_count);
        return out;
    }

    [[nodiscard]] bool is_repetition_sensitive(std::size_t repetitions) const noexcept {
        return repetitions >= 2;
    }

    [[nodiscard]] bool is_repetition_sensitive() const noexcept {
        return is_repetition_sensitive(repetition_count());
    }

    [[nodiscard]] bool is_draw_by_rule() const noexcept {
        return draw_status() != DrawStatus::none;
    }

    [[nodiscard]] bool is_terminal() noexcept {
        const DrawStatus status = draw_status();
        if (status == DrawStatus::dead_position ||
            status == DrawStatus::automatic_fivefold ||
            status == DrawStatus::automatic_seventy_five_move) {
            return true;
        }
        try {
            // Checkmate takes precedence over claimable or automatic draw
            // clocks; otherwise a no-move position is terminal as stalemate.
            return !has_legal_move();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept { return state.halfmove; }
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept { return state.fullmove; }

private:
    template <typename MoveSink>
    [[nodiscard]] bool push(MoveSink& moves, int from, int to) const {
        if (!valid_square(to)) return true;
        return push(moves, from, to, state.board[static_cast<std::size_t>(to)]);
    }

    // Bit-identity: `target` is the value the three-argument overload reloads
    // from `state.board[to]`; a caller that already loaded that same square
    // passes it here, so the checks, the promotion decision, and the emission
    // order below are unchanged.  The return value reports whether the sink
    // wants the walk to continue (the container sinks always do).
    template <typename MoveSink>
    [[nodiscard]] bool push(MoveSink& moves, int from, int to, Piece target) const {
        if (!valid_square(to)) return true;
        if (!target.empty() && target.color == state.side) return true;
        if (target.type == PieceType::king) return true;
        const Square source = Square::from_index(static_cast<std::uint8_t>(from));
        const Square destination = Square::from_index(static_cast<std::uint8_t>(to));
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        const int promotion_rank = moving.color == Color::white ? 7 : 0;
        if (moving.type == PieceType::pawn && rank_of(to) == promotion_rank) {
            return add_promotion_moves(moves, source, destination);
        }
        return moves.emplace_back(source, destination);
    }

    // `tactical_only` emits captures, en-passant, and promotions without quiet
    // moves; `tactical_and_checks` adds non-tactical checking candidates when
    // not in check. Both emit in the exact reference piece/ray order.
    template <PseudoGenMode Mode = PseudoGenMode::all, typename MoveSink>
    void generate_pseudo(MoveSink& moves) {
        const Color side = state.side;
        const std::size_t side_index = side == Color::white ? 0U : 1U;
        const std::size_t enemy_index = 1U - side_index;
        const std::uint64_t checkers = state.checkers[side_index];
        const std::uint64_t pinned = state.pinned[side_index];
        const int king = king_square(state, side);
        const std::uint64_t enemy_king_bb =
            state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::king)];
        if constexpr (Mode == PseudoGenMode::tactical_and_checks) {
            if (checkers != 0 || king < 0 || enemy_king_bb == 0 ||
                (enemy_king_bb & (enemy_king_bb - 1)) != 0) {
                generate_pseudo<PseudoGenMode::all>(moves);
                return;
            }
        }
        const bool double_check = (checkers & (checkers - 1)) != 0;
        const std::uint64_t single_check_targets =
            (checkers != 0 && !double_check && king >= 0)
                ? (checkers | detail::between_mask(king, static_cast<int>(std::countr_zero(checkers))))
                : ~std::uint64_t{0};
        const std::uint64_t friendly = state.occupancy[side_index];
        const std::uint64_t enemy_non_king = state.occupancy[enemy_index] & ~enemy_king_bb;
        constexpr bool tactical_only = Mode == PseudoGenMode::tactical_only;
        constexpr bool tactical_and_checks = Mode == PseudoGenMode::tactical_and_checks;
        constexpr bool quiet_only = Mode == PseudoGenMode::quiet_only;
        const std::uint64_t valid_targets =
            quiet_only ? ~state.occupied
                       : (tactical_only ? enemy_non_king : ~(friendly | enemy_king_bb));

        int ek = -1;
        std::uint64_t direct_pawn_checks = 0;
        std::uint64_t direct_knight_checks = 0;
        std::uint64_t direct_rook_checks = 0;
        std::uint64_t direct_bishop_checks = 0;
        std::uint64_t dc_blockers = 0;
        if constexpr (tactical_and_checks) {
            bool reused_probe = false;
            if constexpr (is_metadata_sink_v<MoveSink>) {
                if (moves.has_check_probe() && moves.enemy_king_square() >= 0) {
                    ek = moves.enemy_king_square();
                    direct_pawn_checks = moves.direct_pawn_checks();
                    direct_knight_checks = moves.direct_knight_checks();
                    direct_rook_checks = moves.rook_rays_pre();
                    direct_bishop_checks = moves.bishop_rays_pre();
                    if (moves.can_fast_direct()) {
                        dc_blockers = moves.dc_blockers();
                        reused_probe = true;
                    }
                }
            }
            if (!reused_probe) {
                const std::uint64_t friendly_rq =
                    state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::rook)] |
                    state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::queen)];
                const std::uint64_t friendly_bq =
                    state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::bishop)] |
                    state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::queen)];
                if (ek < 0) {
                    ek = static_cast<int>(std::countr_zero(enemy_king_bb));
                    direct_pawn_checks = detail::pawn_attackers_of(ek, side == Color::white);
                    direct_knight_checks = detail::knight_attacks(ek);
                    direct_rook_checks = friendly_rq != 0
                        ? detail::rook_attacks(ek, state.occupied)
                        : 0ULL;
                    direct_bishop_checks = friendly_bq != 0
                        ? detail::bishop_attacks(ek, state.occupied)
                        : 0ULL;
                }
                const std::uint64_t rook_snipers = friendly_rq != 0
                    ? (detail::rook_pseudo_attacks(ek) & friendly_rq)
                    : 0ULL;
                const std::uint64_t bishop_snipers = friendly_bq != 0
                    ? (detail::bishop_pseudo_attacks(ek) & friendly_bq)
                    : 0ULL;
                const std::uint64_t rook_blockers =
                    rook_snipers != 0 ? (direct_rook_checks & friendly) : 0ULL;
                const std::uint64_t bishop_blockers =
                    bishop_snipers != 0 ? (direct_bishop_checks & friendly) : 0ULL;
                if (rook_blockers != 0) {
                    std::uint64_t pinners =
                        detail::rook_attacks(ek, state.occupied ^ rook_blockers) & rook_snipers;
                    while (pinners != 0) {
                        const int sniper = static_cast<int>(std::countr_zero(pinners));
                        pinners &= pinners - 1;
                        dc_blockers |= detail::between_mask(ek, sniper) & rook_blockers;
                    }
                }
                if (bishop_blockers != 0) {
                    std::uint64_t pinners =
                        detail::bishop_attacks(ek, state.occupied ^ bishop_blockers) & bishop_snipers;
                    while (pinners != 0) {
                        const int sniper = static_cast<int>(std::countr_zero(pinners));
                        pinners &= pinners - 1;
                        dc_blockers |= detail::between_mask(ek, sniper) & bishop_blockers;
                    }
                }
            }
        }

        if constexpr (is_metadata_sink_v<MoveSink>) {
            {
                if (king >= 0) {
                    const Square king_sq = Square::from_index(static_cast<std::uint8_t>(king));
                    const bool king_safe = !moves.dc_hits(king);
                    const std::uint64_t king_targets_mask = tactical_and_checks
                        ? (enemy_non_king |
                           (~(friendly | enemy_king_bb) &
                            ((dc_blockers & bit(king)) != 0 ? ~detail::line_mask(ek, king) : 0ULL)))
                        : valid_targets;
                    std::uint64_t k_targets = detail::king_attacks(king) & king_targets_mask;
                    while (k_targets != 0) {
                        const int to = static_cast<int>(std::countr_zero(k_targets));
                        k_targets &= k_targets - 1;
                        const Square dest_sq = Square::from_index(static_cast<std::uint8_t>(to));
                        const Move candidate(king_sq, dest_sq);
                        if (king_move_is_legal(candidate)) {
                            const bool cap = !quiet_only &&
                                (tactical_only || ((enemy_non_king & bit(to)) != 0));
                            const PieceType cap_pt = cap ?
                                state.board[static_cast<std::size_t>(to)].type : PieceType::none;
                            if (!moves.emit_piece_move(
                                    king_sq, king, to, PieceType::king, cap_pt,
                                    cap ? MoveKind::capture : MoveKind::quiet, king_safe)) {
                                return;
                            }
                        }
                    }
                    if constexpr (!tactical_only) {
                        if (checkers == 0) {
                            const int rank_base = side == Color::white ? 0 : 7;
                            if (king == rank_base * 8 + 4) {
                                const std::uint64_t rook_lines_empty =
                                    tactical_and_checks ? detail::rook_pseudo_attacks(ek) : 0ULL;
                                const std::uint64_t dc_king =
                                    (tactical_and_checks && (dc_blockers & bit(king)) != 0)
                                        ? ~detail::line_mask(ek, king)
                                        : 0ULL;
                                const std::uint8_t king_right =
                                    side == Color::white ? kWhiteKingSide : kBlackKingSide;
                                const std::uint8_t queen_right =
                                    side == Color::white ? kWhiteQueenSide : kBlackQueenSide;
                                const bool allow_kingside = !tactical_and_checks ||
                                    ((rook_lines_empty & bit(rank_base * 8 + 5)) != 0) ||
                                    dc_king != 0;
                                if ((state.castling & king_right) != 0 && allow_kingside &&
                                    state.board[static_cast<std::size_t>(rank_base * 8 + 5)].empty() &&
                                    state.board[static_cast<std::size_t>(rank_base * 8 + 6)].empty() &&
                                    safe_king_step(state, king, rank_base * 8 + 5, side) &&
                                    safe_king_step(state, king, rank_base * 8 + 6, side)) {
                                    const int to = rank_base * 8 + 6;
                                    const Square dest_sq =
                                        Square::from_index(static_cast<std::uint8_t>(to));
                                    if (!moves.emit_castling(king_sq, dest_sq, king, to)) return;
                                }
                                const bool allow_queenside = !tactical_and_checks ||
                                    ((rook_lines_empty & bit(rank_base * 8 + 3)) != 0) ||
                                    dc_king != 0;
                                if ((state.castling & queen_right) != 0 && allow_queenside &&
                                    state.board[static_cast<std::size_t>(rank_base * 8 + 1)].empty() &&
                                    state.board[static_cast<std::size_t>(rank_base * 8 + 2)].empty() &&
                                    state.board[static_cast<std::size_t>(rank_base * 8 + 3)].empty() &&
                                    safe_king_step(state, king, rank_base * 8 + 3, side) &&
                                    safe_king_step(state, king, rank_base * 8 + 2, side)) {
                                    const int to = rank_base * 8 + 2;
                                    const Square dest_sq =
                                        Square::from_index(static_cast<std::uint8_t>(to));
                                    if (!moves.emit_castling(king_sq, dest_sq, king, to)) return;
                                }
                            }
                        }
                    }
                }
                if (double_check) {
                    return;
                }

                // Pawns (bulk bitboard shifts for unpinned, non-dc-blocker pawns).
                const std::uint64_t all_pawns =
                    state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::pawn)];
                if (all_pawns != 0) {
                    const auto emit_side_pawns = [&]<bool White>() -> bool {
                        constexpr std::uint64_t kFileAMask = 0x0101010101010101ULL;
                        constexpr std::uint64_t kFileHMask = 0x8080808080808080ULL;
                        constexpr std::uint64_t kPromoMask =
                            White ? 0xFF00000000000000ULL : 0x00000000000000FFULL;
                        constexpr std::uint64_t kMidPushMask =
                            White ? 0x0000000000FF0000ULL : 0x0000FF0000000000ULL;
                        constexpr int kPush = White ? 8 : -8;
                        constexpr int kLeftCapDelta = White ? 7 : -9;
                        constexpr int kRightCapDelta = White ? 9 : -7;
                        const auto shift_fwd = [](const std::uint64_t bb) noexcept {
                            return White ? (bb << 8) : (bb >> 8);
                        };

                        const std::uint64_t empty = ~state.occupied;
                        const std::uint64_t slow_mask =
                            pinned | (tactical_and_checks ? dc_blockers : 0ULL);
                        const std::uint64_t fast_pawns = all_pawns & ~slow_mask;
                        if (fast_pawns != 0) {
                            if constexpr (!quiet_only) {
                            const std::uint64_t cap_left_all =
                                (White ? ((fast_pawns & ~kFileAMask) << 7)
                                       : ((fast_pawns & ~kFileAMask) >> 9)) &
                                enemy_non_king & single_check_targets;
                            std::uint64_t cap_left_quiet = cap_left_all & ~kPromoMask;
                            while (cap_left_quiet != 0) {
                                const int to = static_cast<int>(std::countr_zero(cap_left_quiet));
                                cap_left_quiet &= cap_left_quiet - 1;
                                const int from = to - kLeftCapDelta;
                                if (!moves.emit_piece_move(
                                        Square::from_index(static_cast<std::uint8_t>(from)),
                                        from, to, PieceType::pawn,
                                        state.board[static_cast<std::size_t>(to)].type,
                                        MoveKind::capture, moves.capture_from_safe(from))) {
                                    return false;
                                }
                            }
                            std::uint64_t cap_left_promo = cap_left_all & kPromoMask;
                            while (cap_left_promo != 0) {
                                const int to = static_cast<int>(std::countr_zero(cap_left_promo));
                                cap_left_promo &= cap_left_promo - 1;
                                const int from = to - kLeftCapDelta;
                                if (!moves.emit_pawn_move(
                                        Square::from_index(static_cast<std::uint8_t>(from)),
                                        from, to, true,
                                        state.board[static_cast<std::size_t>(to)],
                                        moves.capture_from_safe(from))) {
                                    return false;
                                }
                            }

                            const std::uint64_t cap_right_all =
                                (White ? ((fast_pawns & ~kFileHMask) << 9)
                                       : ((fast_pawns & ~kFileHMask) >> 7)) &
                                enemy_non_king & single_check_targets;
                            std::uint64_t cap_right_quiet = cap_right_all & ~kPromoMask;
                            while (cap_right_quiet != 0) {
                                const int to = static_cast<int>(std::countr_zero(cap_right_quiet));
                                cap_right_quiet &= cap_right_quiet - 1;
                                const int from = to - kRightCapDelta;
                                if (!moves.emit_piece_move(
                                        Square::from_index(static_cast<std::uint8_t>(from)),
                                        from, to, PieceType::pawn,
                                        state.board[static_cast<std::size_t>(to)].type,
                                        MoveKind::capture, moves.capture_from_safe(from))) {
                                    return false;
                                }
                            }
                            std::uint64_t cap_right_promo = cap_right_all & kPromoMask;
                            while (cap_right_promo != 0) {
                                const int to = static_cast<int>(std::countr_zero(cap_right_promo));
                                cap_right_promo &= cap_right_promo - 1;
                                const int from = to - kRightCapDelta;
                                if (!moves.emit_pawn_move(
                                        Square::from_index(static_cast<std::uint8_t>(from)),
                                        from, to, true,
                                        state.board[static_cast<std::size_t>(to)],
                                        moves.capture_from_safe(from))) {
                                    return false;
                                }
                            }
                            }

                            const std::uint64_t push_all = shift_fwd(fast_pawns) & empty;
                            if constexpr (!quiet_only) {
                            std::uint64_t push_promo = push_all & kPromoMask & single_check_targets;
                            while (push_promo != 0) {
                                const int to = static_cast<int>(std::countr_zero(push_promo));
                                push_promo &= push_promo - 1;
                                const int from = to - kPush;
                                if (!moves.emit_pawn_move(
                                        Square::from_index(static_cast<std::uint8_t>(from)),
                                        from, to, true, Piece{}, moves.capture_from_safe(from))) {
                                    return false;
                                }
                            }
                            }

                            if constexpr (!tactical_only) {
                                const std::uint64_t b1_all = push_all & ~kPromoMask;
                                const std::uint64_t b2_all =
                                    shift_fwd(b1_all & kMidPushMask) & empty;
                                const std::uint64_t quiet_filter = tactical_and_checks
                                    ? (single_check_targets & direct_pawn_checks)
                                    : single_check_targets;
                                std::uint64_t b1_emit = b1_all & quiet_filter;
                                while (b1_emit != 0) {
                                    const int to = static_cast<int>(std::countr_zero(b1_emit));
                                    b1_emit &= b1_emit - 1;
                                    const int from = to - kPush;
                                    if (!moves.emit_piece_move(
                                            Square::from_index(static_cast<std::uint8_t>(from)),
                                            from, to, PieceType::pawn, PieceType::none,
                                            MoveKind::quiet, !moves.dc_hits(from))) {
                                        return false;
                                    }
                                }
                                std::uint64_t b2_emit = b2_all & quiet_filter;
                                while (b2_emit != 0) {
                                    const int to = static_cast<int>(std::countr_zero(b2_emit));
                                    b2_emit &= b2_emit - 1;
                                    const int from = to - (2 * kPush);
                                    if (!moves.emit_piece_move(
                                            Square::from_index(static_cast<std::uint8_t>(from)),
                                            from, to, PieceType::pawn, PieceType::none,
                                            MoveKind::quiet, !moves.dc_hits(from))) {
                                        return false;
                                    }
                                }
                            }
                        }

                        std::uint64_t slow_pawns = all_pawns & slow_mask;
                        while (slow_pawns != 0) {
                            const int from = static_cast<int>(std::countr_zero(slow_pawns));
                            slow_pawns &= slow_pawns - 1;
                            const bool is_pinned = (pinned & bit(from)) != 0;
                            if (is_pinned && checkers != 0) {
                                continue;
                            }
                            const Square source = Square::from_index(static_cast<std::uint8_t>(from));
                            const bool from_safe = !moves.dc_hits(from);
                            const std::uint64_t dc_off_ray =
                                (tactical_and_checks && (dc_blockers & bit(from)) != 0)
                                    ? ~detail::line_mask(ek, from)
                                    : 0ULL;
                            const std::uint64_t pawn_legal_targets = is_pinned && king >= 0
                                ? (single_check_targets & detail::line_mask(king, from))
                                : single_check_targets;
                            const int rank = rank_of(from);
                            const int file = file_of(from);
                            const int direction = White ? 1 : -1;
                            const int promo_rank = White ? 7 : 0;
                            const int start_rank = White ? 1 : 6;
                            const int one = from + direction * 8;
                            if (valid_square(one) &&
                                state.board[static_cast<std::size_t>(one)].empty()) {
                                const bool is_promo = rank_of(one) == promo_rank;
                                if constexpr (!tactical_only) {
                                    const std::uint64_t quiet_mask =
                                        direct_pawn_checks | dc_off_ray;
                                    if ((!quiet_only || !is_promo) &&
                                        (pawn_legal_targets & bit(one)) != 0 &&
                                        (!tactical_and_checks || is_promo ||
                                         (quiet_mask & bit(one)) != 0)) {
                                        if (!moves.emit_pawn_move(
                                                source, from, one, is_promo, Piece{}, from_safe)) {
                                            return false;
                                        }
                                    }
                                    const int two = from + direction * 16;
                                    if (rank == start_rank &&
                                        state.board[static_cast<std::size_t>(two)].empty() &&
                                        (pawn_legal_targets & bit(two)) != 0 &&
                                        (!tactical_and_checks || (quiet_mask & bit(two)) != 0)) {
                                        if (!moves.emit_piece_move(
                                                source, from, two, PieceType::pawn,
                                                PieceType::none, MoveKind::quiet, from_safe)) {
                                            return false;
                                        }
                                    }
                                } else if (is_promo && (pawn_legal_targets & bit(one)) != 0) {
                                    if (!moves.emit_pawn_move(
                                            source, from, one, true, Piece{}, from_safe)) {
                                        return false;
                                    }
                                }
                            }
                            if constexpr (!quiet_only) {
                            for (const int file_delta : {-1, 1}) {
                                const int target_file = file + file_delta;
                                const int target_rank = rank + direction;
                                if (target_file < 0 || target_file >= 8 ||
                                    target_rank < 0 || target_rank >= 8) {
                                    continue;
                                }
                                const int to = target_rank * 8 + target_file;
                                if ((pawn_legal_targets & bit(to)) == 0) {
                                    continue;
                                }
                                const Piece target = state.board[static_cast<std::size_t>(to)];
                                if (!target.empty() && target.color != side &&
                                    target.type != PieceType::king) {
                                    if (!moves.emit_pawn_move(
                                            source, from, to, target_rank == promo_rank, target,
                                            from_safe)) {
                                        return false;
                                    }
                                }
                            }
                            }
                        }

                        if constexpr (!quiet_only) {
                        if (state.en_passant.index() < Square::kInvalid) {
                            const int ep_to = state.en_passant.index();
                            const Piece ep_target_piece =
                                state.board[static_cast<std::size_t>(ep_to)];
                            if (ep_target_piece.empty()) {
                                const Square ep_sq =
                                    Square::from_index(static_cast<std::uint8_t>(ep_to));
                                std::uint64_t ep_sources =
                                    all_pawns & detail::pawn_attackers_of(ep_to, White);
                                while (ep_sources != 0) {
                                    const int from =
                                        static_cast<int>(std::countr_zero(ep_sources));
                                    ep_sources &= ep_sources - 1;
                                    const Square source =
                                        Square::from_index(static_cast<std::uint8_t>(from));
                                    const Move ep_move(source, ep_sq);
                                    if (resolve_move_legality(ep_move, side)) {
                                        if (!moves.emit_en_passant(source, ep_sq, from, ep_to)) {
                                            return false;
                                        }
                                    }
                                }
                            }
                        }
                        }
                        return true;
                    };

                    if (side == Color::white) {
                        if (!emit_side_pawns.template operator()<true>()) return;
                    } else {
                        if (!emit_side_pawns.template operator()<false>()) return;
                    }
                }

                const auto emit_split_targets = [&]<PieceType PT>(
                    const Square source, const int from,
                    const std::uint64_t targets, const bool from_safe) noexcept -> bool {
                    if constexpr (!quiet_only) {
                        std::uint64_t cap_targets = tactical_only ?
                            targets : (targets & enemy_non_king);
                        while (cap_targets != 0) {
                            const int to = static_cast<int>(std::countr_zero(cap_targets));
                            cap_targets &= cap_targets - 1;
                            if (!moves.emit_piece_move(
                                    source, from, to, PT,
                                    state.board[static_cast<std::size_t>(to)].type,
                                    MoveKind::capture, from_safe)) {
                                return false;
                            }
                        }
                    }
                    if constexpr (!tactical_only) {
                        std::uint64_t quiet_targets =
                            quiet_only ? targets : (targets & ~enemy_non_king);
                        while (quiet_targets != 0) {
                            const int to = static_cast<int>(std::countr_zero(quiet_targets));
                            quiet_targets &= quiet_targets - 1;
                            if (!moves.emit_piece_move(
                                    source, from, to, PT, PieceType::none,
                                    MoveKind::quiet, from_safe)) {
                                return false;
                            }
                        }
                    }
                    return true;
                };

                // Knights (pinned knights have zero legal moves).
                std::uint64_t knights =
                    state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::knight)] &
                    ~pinned;
                if (knights != 0) {
                    const std::uint64_t quiet_base = tactical_and_checks
                        ? (~(friendly | enemy_king_bb) & direct_knight_checks)
                        : 0ULL;
                    const std::uint64_t base_mask = (tactical_and_checks
                        ? (enemy_non_king | quiet_base)
                        : valid_targets) & single_check_targets;
                    while (knights != 0) {
                        const int from = static_cast<int>(std::countr_zero(knights));
                        knights &= knights - 1;
                        std::uint64_t mask = base_mask;
                        if constexpr (tactical_and_checks) {
                            if ((dc_blockers & bit(from)) != 0) {
                                mask |= ~(friendly | enemy_king_bb) & ~detail::line_mask(ek, from) &
                                    single_check_targets;
                            }
                        }
                        const std::uint64_t targets = detail::knight_attacks(from) & mask;
                        if (targets == 0) continue;
                        const Square source = Square::from_index(static_cast<std::uint8_t>(from));
                        const bool from_safe = !moves.dc_hits(from);
                        if (!emit_split_targets.template operator()<PieceType::knight>(
                                source, from, targets, from_safe)) {
                            return;
                        }
                    }
                }

                const auto emit_sliders = [&]<PieceType PT, bool IsDiag, bool IsOrth>(
                    const std::uint64_t direct_checks_mask) -> bool {
                    std::uint64_t pieces =
                        state.piece_bitboards[side_index][static_cast<std::size_t>(PT)];
                    if (checkers != 0) {
                        pieces &= ~pinned;
                    }
                    const std::uint64_t base_targets = (tactical_and_checks
                        ? (enemy_non_king | (~(friendly | enemy_king_bb) & direct_checks_mask))
                        : valid_targets) & single_check_targets;
                    while (pieces != 0) {
                        const int from = static_cast<int>(std::countr_zero(pieces));
                        pieces &= pieces - 1;
                        std::uint64_t slider_mask = base_targets;
                        if constexpr (tactical_and_checks) {
                            if ((dc_blockers & bit(from)) != 0) {
                                slider_mask |= ~(friendly | enemy_king_bb) &
                                    ~detail::line_mask(ek, from) & single_check_targets;
                            }
                        }
                        if ((pinned & bit(from)) != 0 && king >= 0) {
                            slider_mask &= detail::line_mask(king, from);
                        }
                        if (slider_mask == 0) continue;
                        std::uint64_t slider_targets = 0;
                        if constexpr (IsDiag) {
                            slider_targets |= detail::bishop_attacks(from, state.occupied);
                        }
                        if constexpr (IsOrth) {
                            slider_targets |= detail::rook_attacks(from, state.occupied);
                        }
                        slider_targets &= slider_mask;
                        if (slider_targets == 0) continue;
                        const Square source = Square::from_index(static_cast<std::uint8_t>(from));
                        const bool from_safe = !moves.dc_hits(from);
                        if (!emit_split_targets.template operator()<PT>(
                                source, from, slider_targets, from_safe)) {
                            return false;
                        }
                    }
                    return true;
                };

                if (!emit_sliders.template operator()<PieceType::bishop, true, false>(
                        direct_bishop_checks)) {
                    return;
                }
                if (!emit_sliders.template operator()<PieceType::rook, false, true>(
                        direct_rook_checks)) {
                    return;
                }
                if (!emit_sliders.template operator()<PieceType::queen, true, true>(
                        direct_bishop_checks | direct_rook_checks)) {
                    return;
                }
                return;
            }
        }

        if constexpr (quiet_only) {
            return;
        }
        std::uint64_t origins = double_check ?
            state.piece_bitboards[side_index][static_cast<std::size_t>(PieceType::king)] :
            friendly;
        while (origins != 0) {
            const int from = static_cast<int>(std::countr_zero(origins));
            origins &= origins - 1;
            const Piece piece = state.board[static_cast<std::size_t>(from)];
            if (piece.empty() || piece.color != side) continue;
            const bool is_pinned = (pinned & bit(from)) != 0;
            const Square source = Square::from_index(static_cast<std::uint8_t>(from));
            bool from_safe = true;
            if constexpr (is_metadata_sink_v<MoveSink>) {
                from_safe = !moves.dc_hits(from);
            }
            std::uint64_t dc_off_ray = 0;
            if constexpr (tactical_and_checks) {
                if ((dc_blockers & bit(from)) != 0) {
                    dc_off_ray = ~detail::line_mask(ek, from);
                }
            }

            if (piece.type == PieceType::pawn) {
                const int file = file_of(from);
                const int rank = rank_of(from);
                const int direction = side == Color::white ? 1 : -1;
                const std::uint64_t pawn_legal_targets =
                    (is_pinned && checkers != 0) ? std::uint64_t{0} :
                    (is_pinned && king >= 0 ?
                        (single_check_targets & detail::line_mask(king, from)) :
                        single_check_targets);

                const int one = from + direction * 8;
                if (valid_square(one)) {
                    const Piece one_target = state.board[static_cast<std::size_t>(one)];
                    if (one_target.empty()) {
                        if constexpr (!tactical_only) {
                            const int promotion_rank = side == Color::white ? 7 : 0;
                            const bool is_promotion = rank_of(one) == promotion_rank;
                            const std::uint64_t pawn_quiet_mask =
                                direct_pawn_checks | dc_off_ray;
                            if ((pawn_legal_targets & bit(one)) != 0 &&
                                (!tactical_and_checks || is_promotion ||
                                 (pawn_quiet_mask & bit(one)) != 0)) {
                                if constexpr (is_metadata_sink_v<MoveSink>) {
                                    if (!moves.emit_pawn_move(source, from, one, is_promotion, one_target, from_safe)) return;
                                } else {
                                    if (!push(moves, from, one, one_target)) return;
                                }
                            }
                            const int two = from + direction * 16;
                            const int start_rank = side == Color::white ? 1 : 6;
                            if (rank == start_rank &&
                                state.board[static_cast<std::size_t>(two)].empty() &&
                                (pawn_legal_targets & bit(two)) != 0 &&
                                (!tactical_and_checks || (pawn_quiet_mask & bit(two)) != 0)) {
                                if constexpr (is_metadata_sink_v<MoveSink>) {
                                    if (!moves.emit_piece_move(source, from, two, PieceType::pawn,
                                                               PieceType::none, MoveKind::quiet,
                                                               from_safe)) {
                                        return;
                                    }
                                } else {
                                    if (!moves.emplace_back(
                                            source,
                                            Square::from_index(static_cast<std::uint8_t>(two)))) {
                                        return;
                                    }
                                }
                            }
                        } else {
                            const int promotion_rank = side == Color::white ? 7 : 0;
                            if (rank_of(one) == promotion_rank &&
                                (pawn_legal_targets & bit(one)) != 0) {
                                if constexpr (is_metadata_sink_v<MoveSink>) {
                                    if (!moves.emit_pawn_move(source, from, one, true, one_target, from_safe)) return;
                                } else {
                                    if (!push(moves, from, one, one_target)) return;
                                }
                            }
                        }
                    }
                }
                for (const int file_delta : {-1, 1}) {
                    const int target_file = file + file_delta;
                    const int target_rank = rank + direction;
                    if (target_file < 0 || target_file >= 8 || target_rank < 0 || target_rank >= 8) continue;
                    const int to = target_rank * 8 + target_file;
                    const Piece target = state.board[static_cast<std::size_t>(to)];
                    if (!target.empty() && target.color != side && target.type != PieceType::king) {
                        if ((pawn_legal_targets & bit(to)) != 0) {
                            if constexpr (is_metadata_sink_v<MoveSink>) {
                                const int promotion_rank = side == Color::white ? 7 : 0;
                                if (!moves.emit_pawn_move(source, from, to,
                                                          target_rank == promotion_rank,
                                                          target, from_safe)) {
                                    return;
                                }
                            } else {
                                if (!push(moves, from, to, target)) return;
                            }
                        }
                    } else if (state.en_passant.index() == to && target.empty()) {
                        const Square ep_sq = Square::from_index(static_cast<std::uint8_t>(to));
                        const Move ep_move(source, ep_sq);
                        if (resolve_move_legality(ep_move, side)) {
                            if constexpr (is_metadata_sink_v<MoveSink>) {
                                if (!moves.emit_en_passant(source, ep_sq, from, to)) return;
                            } else {
                                if (!moves.emplace_back(ep_move)) return;
                            }
                        }
                    }
                }
                continue;
            }
            if (piece.type == PieceType::knight) {
                if (is_pinned) continue;
                const std::uint64_t knight_targets_mask = tactical_and_checks
                    ? (enemy_non_king |
                       (~(friendly | enemy_king_bb) & (direct_knight_checks | dc_off_ray)))
                    : valid_targets;
                std::uint64_t targets =
                    detail::knight_attacks(from) & knight_targets_mask & single_check_targets;
                if (targets == 0) continue;
                if constexpr (is_metadata_sink_v<MoveSink>) {
                    while (targets != 0) {
                        const int to = static_cast<int>(std::countr_zero(targets));
                        targets &= targets - 1;
                        const bool cap = tactical_only || ((enemy_non_king & bit(to)) != 0);
                        const PieceType cap_pt = cap ?
                            state.board[static_cast<std::size_t>(to)].type : PieceType::none;
                        if (!moves.emit_piece_move(
                                source, from, to, PieceType::knight, cap_pt,
                                cap ? MoveKind::capture : MoveKind::quiet, from_safe)) {
                            return;
                        }
                    }
                } else {
                    static constexpr std::array<int, 8> kKnightStepDeltas{
                        17, 10, -6, -15, -17, -10, 6, 15};
                    for (const int delta : kKnightStepDeltas) {
                        const int to = from + delta;
                        if (static_cast<unsigned>(to) < 64U && (targets & bit(to)) != 0) {
                            if (!moves.emplace_back(
                                    source, Square::from_index(static_cast<std::uint8_t>(to)))) {
                                return;
                            }
                        }
                    }
                }
                continue;
            }
            if (piece.type == PieceType::king) {
                const std::uint64_t king_targets_mask = tactical_and_checks
                    ? (enemy_non_king | (~(friendly | enemy_king_bb) & dc_off_ray))
                    : valid_targets;
                std::uint64_t targets = detail::king_attacks(from) & king_targets_mask;
                if (targets != 0) {
                    if constexpr (is_metadata_sink_v<MoveSink>) {
                        while (targets != 0) {
                            const int to = static_cast<int>(std::countr_zero(targets));
                            targets &= targets - 1;
                            const Square dest_sq =
                                Square::from_index(static_cast<std::uint8_t>(to));
                            const Move candidate(source, dest_sq);
                            if (king_move_is_legal(candidate)) {
                                const bool cap = tactical_only || ((enemy_non_king & bit(to)) != 0);
                                const PieceType cap_pt = cap ?
                                    state.board[static_cast<std::size_t>(to)].type : PieceType::none;
                                if (!moves.emit_piece_move(
                                        source, from, to, PieceType::king, cap_pt,
                                        cap ? MoveKind::capture : MoveKind::quiet, from_safe)) {
                                    return;
                                }
                            }
                        }
                    } else {
                        static constexpr std::array<int, 8> kKingStepDeltas{
                            9, 1, -7, -8, -9, -1, 7, 8};
                        for (const int delta : kKingStepDeltas) {
                            const int to = from + delta;
                            if (static_cast<unsigned>(to) < 64U && (targets & bit(to)) != 0) {
                                const Square dest_sq =
                                    Square::from_index(static_cast<std::uint8_t>(to));
                                const Move candidate(source, dest_sq);
                                if (king_move_is_legal(candidate)) {
                                    if (!moves.emplace_back(candidate)) return;
                                }
                            }
                        }
                    }
                }
                if constexpr (!tactical_only) {
                    if (king >= 0 && checkers == 0) {
                        const int rank_base = side == Color::white ? 0 : 7;
                        if (from == rank_base * 8 + 4) {
                            const std::uint64_t rook_lines_empty =
                                tactical_and_checks ? detail::rook_pseudo_attacks(ek) : 0ULL;
                            const std::uint8_t king_right = side == Color::white ? kWhiteKingSide : kBlackKingSide;
                            const std::uint8_t queen_right = side == Color::white ? kWhiteQueenSide : kBlackQueenSide;
                            const bool allow_kingside = !tactical_and_checks ||
                                ((rook_lines_empty & bit(rank_base * 8 + 5)) != 0) ||
                                dc_off_ray != 0;
                            if ((state.castling & king_right) != 0 &&
                                allow_kingside &&
                                state.board[static_cast<std::size_t>(rank_base * 8 + 5)].empty() &&
                                state.board[static_cast<std::size_t>(rank_base * 8 + 6)].empty() &&
                                safe_king_step(state, from, rank_base * 8 + 5, side) &&
                                safe_king_step(state, from, rank_base * 8 + 6, side)) {
                                const int to = rank_base * 8 + 6;
                                const Square dest_sq = Square::from_index(static_cast<std::uint8_t>(to));
                                if constexpr (is_metadata_sink_v<MoveSink>) {
                                    if (!moves.emit_castling(source, dest_sq, from, to)) return;
                                } else {
                                    if (!moves.emplace_back(source, dest_sq)) return;
                                }
                            }
                            const bool allow_queenside = !tactical_and_checks ||
                                ((rook_lines_empty & bit(rank_base * 8 + 3)) != 0) ||
                                dc_off_ray != 0;
                            if ((state.castling & queen_right) != 0 &&
                                allow_queenside &&
                                state.board[static_cast<std::size_t>(rank_base * 8 + 1)].empty() &&
                                state.board[static_cast<std::size_t>(rank_base * 8 + 2)].empty() &&
                                state.board[static_cast<std::size_t>(rank_base * 8 + 3)].empty() &&
                                safe_king_step(state, from, rank_base * 8 + 3, side) &&
                                safe_king_step(state, from, rank_base * 8 + 2, side)) {
                                const int to = rank_base * 8 + 2;
                                const Square dest_sq = Square::from_index(static_cast<std::uint8_t>(to));
                                if constexpr (is_metadata_sink_v<MoveSink>) {
                                    if (!moves.emit_castling(source, dest_sq, from, to)) return;
                                } else {
                                    if (!moves.emplace_back(source, dest_sq)) return;
                                }
                            }
                        }
                    }
                }
                continue;
            }

            if (is_pinned && checkers != 0) continue;
            std::uint64_t slider_base_targets = valid_targets;
            if constexpr (tactical_and_checks) {
                const std::uint64_t piece_direct_checks =
                    piece.type == PieceType::bishop
                        ? direct_bishop_checks
                        : (piece.type == PieceType::rook
                               ? direct_rook_checks
                               : (direct_bishop_checks | direct_rook_checks));
                slider_base_targets = enemy_non_king |
                    (~(friendly | enemy_king_bb) & (piece_direct_checks | dc_off_ray));
            }
            std::uint64_t slider_mask = slider_base_targets & single_check_targets;
            if (is_pinned && king >= 0) {
                slider_mask &= detail::line_mask(king, from);
            }
            if (slider_mask == 0) continue;

            const auto emit_ray_dir = [&](int dir, bool positive_step, std::uint64_t piece_targets) -> bool {
                std::uint64_t ray_targets = piece_targets & detail::ray_attacks(from, dir);
                while (ray_targets != 0) {
                    const int to = positive_step ?
                        static_cast<int>(std::countr_zero(ray_targets)) :
                        63 - static_cast<int>(std::countl_zero(ray_targets));
                    ray_targets &= ~bit(to);
                    if constexpr (is_metadata_sink_v<MoveSink>) {
                        const bool cap = tactical_only || ((enemy_non_king & bit(to)) != 0);
                        const PieceType cap_pt = cap ?
                            state.board[static_cast<std::size_t>(to)].type : PieceType::none;
                        if (!moves.emit_piece_move(
                                source, from, to, piece.type, cap_pt,
                                cap ? MoveKind::capture : MoveKind::quiet, from_safe)) {
                            return false;
                        }
                    } else {
                        if (!moves.emplace_back(
                                source, Square::from_index(static_cast<std::uint8_t>(to)))) {
                            return false;
                        }
                    }
                }
                return true;
            };

            const bool diagonal = piece.type == PieceType::bishop || piece.type == PieceType::queen;
            const bool orthogonal = piece.type == PieceType::rook || piece.type == PieceType::queen;
            if constexpr (is_metadata_sink_v<MoveSink>) {
                std::uint64_t slider_targets = 0;
                if (diagonal) {
                    slider_targets |= detail::bishop_attacks(from, state.occupied);
                }
                if (orthogonal) {
                    slider_targets |= detail::rook_attacks(from, state.occupied);
                }
                slider_targets &= slider_mask;
                while (slider_targets != 0) {
                    const int to = static_cast<int>(std::countr_zero(slider_targets));
                    slider_targets &= slider_targets - 1;
                    const bool cap = tactical_only || ((enemy_non_king & bit(to)) != 0);
                    const PieceType cap_pt = cap ?
                        state.board[static_cast<std::size_t>(to)].type : PieceType::none;
                    if (!moves.emit_piece_move(
                            source, from, to, piece.type, cap_pt,
                            cap ? MoveKind::capture : MoveKind::quiet, from_safe)) {
                        return;
                    }
                }
                continue;
            }
            if (diagonal) {
                const std::uint64_t diag_targets =
                    detail::bishop_attacks(from, state.occupied) & slider_mask;
                if (diag_targets != 0) {
                    if (!emit_ray_dir(1, true, diag_targets) ||
                        !emit_ray_dir(3, false, diag_targets) ||
                        !emit_ray_dir(5, false, diag_targets) ||
                        !emit_ray_dir(7, true, diag_targets)) {
                        return;
                    }
                }
            }
            if (orthogonal) {
                const std::uint64_t orth_targets =
                    detail::rook_attacks(from, state.occupied) & slider_mask;
                if (orth_targets != 0) {
                    if (!emit_ray_dir(2, true, orth_targets) ||
                        !emit_ray_dir(0, true, orth_targets) ||
                        !emit_ray_dir(6, false, orth_targets) ||
                        !emit_ray_dir(4, false, orth_targets)) {
                        return;
                    }
                }
            }
        }
    }

    void record_and_apply_unchecked(const Move& move) noexcept {
        UndoRecord* slot = push_undo_record(state);
        const int from = move.from().index();
        const int to = move.to().index();
        const std::uint64_t previous_position_key = state.key;
        const std::uint8_t previous_castling = state.castling;
        // One guarded fetch for the whole make: every incremental helper
        // below would otherwise pay the function-local static guard on its
        // own zobrist() call, and the table never changes mid-make.
        const ZobristKeys& keys = zobrist();
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        const Piece captured = state.board[static_cast<std::size_t>(to)];
        const bool pawn_move = moving.type == PieceType::pawn;
        const bool castling = moving.type == PieceType::king && std::abs(file_of(to) - file_of(from)) == 2;
        const bool en_passant = pawn_move && to == state.en_passant.index() && captured.empty();
        const bool double_push = pawn_move && std::abs(to - from) == 16;
        const bool capture = !captured.empty() || en_passant;
        const int en_passant_captured_square =
            en_passant ? to + (moving.color == Color::white ? -8 : 8) : -1;
        const Piece ep_captured_piece = en_passant
            ? state.board[static_cast<std::size_t>(en_passant_captured_square)]
            : Piece{};

        std::uint8_t flags = 0;
        if (en_passant) flags |= kEpFlag;
        if (castling) flags |= kCastlingFlag;
        if (double_push) flags |= kDoublePushFlag;
        if (capture) flags |= kCaptureFlag;

        slot->key = previous_position_key;
        slot->null_move = false;
        slot->prior_suppressed = state.repetition_history_suppressed;
        slot->prior_halfmove = state.halfmove;
        slot->prior_fullmove = state.fullmove;
        slot->prior_castling = previous_castling;
        slot->prior_en_passant = state.en_passant.index();
        slot->fingerprint = state.repetition_history_fingerprint;
        slot->checkers = state.checkers;
        slot->pinned = state.pinned;
        slot->move = move;
        slot->captured = en_passant ? ep_captured_piece : captured;
        slot->ep_victim = en_passant ? static_cast<std::uint8_t>(en_passant_captured_square) : Square::kInvalid;
        slot->flags = flags;

        // Remove only the rule-state terms that can actually change across
        // this transition, accumulating the new Zobrist hash in a local register
        // instead of bouncing `state.key` back through memory per piece update.
        std::uint64_t next_key = previous_position_key ^ keys.black_to_move;
        if (slot->prior_en_passant < Square::kInvalid &&
            has_legal_en_passant_capture(state)) {
            next_key ^= keys.en_passant[slot->prior_en_passant & 7];
        }
        const std::size_t mover_color_index = moving.color == Color::white ? 0U : 1U;
        if (!moving.empty()) {
            remove_bitboard_piece_known(state, moving, from);
            next_key ^= keys.pieces[mover_color_index][static_cast<std::size_t>(moving.type)]
                                   [static_cast<std::size_t>(from)];
        }
        if (!captured.empty()) {
            remove_bitboard_piece_known(state, captured, to);
            next_key ^= keys.pieces[1U - mover_color_index]
                                   [static_cast<std::size_t>(captured.type)]
                                   [static_cast<std::size_t>(to)];
        }
        if (en_passant) [[unlikely]] {
            if (!ep_captured_piece.empty() && valid_square(en_passant_captured_square)) {
                remove_bitboard_piece_known(state, ep_captured_piece, en_passant_captured_square);
                next_key ^= keys.pieces[1U - mover_color_index]
                                       [static_cast<std::size_t>(ep_captured_piece.type)]
                                       [static_cast<std::size_t>(en_passant_captured_square)];
            }
        }

        state.en_passant = {};
        state.board[static_cast<std::size_t>(from)] = {};
        if (en_passant) [[unlikely]] {
            state.board[static_cast<std::size_t>(en_passant_captured_square)] = {};
        }
        Piece placed = moving;
        if (move.promotion() != Promotion::none) [[unlikely]] {
            PieceType promoted = PieceType::queen;
            if (move.promotion() == Promotion::rook) promoted = PieceType::rook;
            if (move.promotion() == Promotion::bishop) promoted = PieceType::bishop;
            if (move.promotion() == Promotion::knight) promoted = PieceType::knight;
            placed.type = promoted;
        }
        state.board[static_cast<std::size_t>(to)] = placed;
        if (!placed.empty()) {
            add_bitboard_piece_known(state, placed, to);
            next_key ^= keys.pieces[mover_color_index][static_cast<std::size_t>(placed.type)]
                                   [static_cast<std::size_t>(to)];
        }
        if (castling) [[unlikely]] {
            const int rook_from = to > from ? from + 3 : from - 4;
            const int rook_to = to > from ? from + 1 : from - 1;
            const Piece rook = state.board[static_cast<std::size_t>(rook_from)];
            if (!rook.empty()) {
                remove_bitboard_piece_known(state, rook, rook_from);
                next_key ^= keys.pieces[mover_color_index][static_cast<std::size_t>(rook.type)]
                                       [static_cast<std::size_t>(rook_from)];
                add_bitboard_piece_known(state, rook, rook_to);
                next_key ^= keys.pieces[mover_color_index][static_cast<std::size_t>(rook.type)]
                                       [static_cast<std::size_t>(rook_to)];
            }
            state.board[static_cast<std::size_t>(rook_to)] = rook;
            state.board[static_cast<std::size_t>(rook_from)] = {};
        }

        if (previous_castling != 0) {
            static constexpr std::array<std::uint8_t, 64> kCastlingRightsMask = []() constexpr {
                std::array<std::uint8_t, 64> m{};
                m.fill(0xFFU);
                m[0] = static_cast<std::uint8_t>(~kWhiteQueenSide);
                m[4] = static_cast<std::uint8_t>(~(kWhiteKingSide | kWhiteQueenSide));
                m[7] = static_cast<std::uint8_t>(~kWhiteKingSide);
                m[56] = static_cast<std::uint8_t>(~kBlackQueenSide);
                m[60] = static_cast<std::uint8_t>(~(kBlackKingSide | kBlackQueenSide));
                m[63] = static_cast<std::uint8_t>(~kBlackKingSide);
                return m;
            }();
            state.castling &= kCastlingRightsMask[static_cast<std::size_t>(from)] &
                kCastlingRightsMask[static_cast<std::size_t>(to)];
            if (state.castling != previous_castling) {
                next_key ^= keys.castling[previous_castling & kAllCastling] ^
                    keys.castling[state.castling & kAllCastling];
            }
        }
        state.halfmove = pawn_move || capture ? 0 :
            static_cast<std::uint16_t>(std::min<unsigned>(255, state.halfmove + 1));
        if (state.side == Color::black) {
            state.fullmove = static_cast<std::uint16_t>(std::min<unsigned>(32768, state.fullmove + 1));
        }
        state.side = opposite(state.side);
        state.occupied = state.occupancy[0] | state.occupancy[1];
        if (double_push) [[unlikely]] {
            state.en_passant = Square::from_index(static_cast<std::uint8_t>((to + from) / 2));
            if (has_legal_en_passant_capture(state)) {
                next_key ^= keys.en_passant[state.en_passant.index() & 7];
            } else {
                state.en_passant = {};
            }
        }
        state.key = next_key;
        const bool irreversible = pawn_move || capture || state.castling != previous_castling;
        if (!state.repetition_history_suppressed) {
            state.repetition_history_fingerprint = irreversible ? 0 :
                extend_repetition_history_fingerprint(
                    state.repetition_history_fingerprint, previous_position_key);
        } else if (irreversible) {
            // Do not let a real move made after a search null move turn the
            // artificial null branch into a legal repetition ancestor. This
            // irreversible move starts a fresh real-history segment, so later
            // reversible moves must be tracked again.
            state.repetition_history_fingerprint = 0;
            state.repetition_history_suppressed = false;
        }
    }
};

} // namespace

class Position::Impl {
public:
    NativePosition position;
};

Position::Position() : impl_(std::make_unique<Impl>()) {}

Position::Position(std::string_view fen) : impl_(std::make_unique<Impl>()) {
    if (!impl_->position.set_fen(fen, true)) throw std::invalid_argument("invalid FEN");
}

Position::Position(const Position& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Position::Position(Position&&) noexcept = default;

Position& Position::operator=(const Position& other) {
    if (this != &other) *impl_ = *other.impl_;
    return *this;
}

Position& Position::operator=(Position&&) noexcept = default;
Position::~Position() = default;

std::string Position::fen() const { return impl_->position.fen(); }

Color Position::side_to_move() const noexcept { return impl_->position.side_to_move(); }

Piece Position::piece_at(Square square) const noexcept { return impl_->position.piece_at(square); }

void Position::copy_board_to(std::span<Piece, 64> output) const noexcept {
    std::ranges::copy(impl_->position.state.board, output.begin());
}

const std::array<Piece, 64>& Position::board_squares() const noexcept {
    return impl_->position.state.board;
}

std::vector<Move> Position::legal_moves() const {
    return const_cast<NativePosition&>(impl_->position).legal_moves();
}

std::size_t Position::legal_moves_into(std::span<Move> output) const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).legal_moves_into(output);
    } catch (...) {
        return 0;
    }
}

std::size_t Position::count_legal_moves_up_to(const std::size_t limit) const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).count_legal_moves_up_to(limit);
    } catch (...) {
        return 0;
    }
}

std::size_t Position::legal_tactical_moves_into(std::span<Move> output) const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).legal_tactical_moves_into(output);
    } catch (...) {
        return 0;
    }
}

std::size_t Position::legal_tactical_and_checking_moves_into(
    std::span<Move> output) const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position)
            .legal_tactical_and_checking_moves_into(output);
    } catch (...) {
        return 0;
    }
}

void Position::legal_search_metadata_into(
    MoveMetadataList& output, const bool include_check_flags,
    const bool quiet_checks_only) const noexcept {
    const_cast<NativePosition&>(impl_->position)
        .legal_search_metadata_into(output, include_check_flags, quiet_checks_only);
}

bool Position::legal_tactical_search_metadata_into(
    MoveMetadataList& output, const bool include_quiet_checks,
    const bool known_has_legal_move) const noexcept {
    return const_cast<NativePosition&>(impl_->position)
        .legal_tactical_search_metadata_into(
            output, include_quiet_checks, known_has_legal_move);
}

void Position::legal_tactical_only_search_metadata_into(
    MoveMetadataList& output) const noexcept {
    const_cast<NativePosition&>(impl_->position)
        .legal_tactical_only_search_metadata_into(output);
}

void Position::append_quiet_search_metadata_into(
    MoveMetadataList& output) const noexcept {
    const_cast<NativePosition&>(impl_->position)
        .append_quiet_search_metadata_into(output);
}

bool Position::has_legal_move() const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).has_legal_move();
    } catch (...) {
        return false;
    }
}

bool Position::has_legal_checking_move() const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).has_legal_checking_move();
    } catch (...) {
        return false;
    }
}

bool Position::is_legal(const Move& move) const noexcept { return impl_->position.is_legal(move); }

bool Position::is_capture(const Move& move) const noexcept { return impl_->position.is_capture(move); }

Position::MoveFacts Position::move_facts(const Move& move) const noexcept {
    return impl_->position.move_facts(move);
}

bool Position::try_legal_move_facts(const Move& move, MoveFacts& facts) const noexcept {
    return impl_->position.try_legal_move_facts(move, facts);
}

bool Position::make_move(const Move& move) noexcept { return impl_->position.make_move(move); }

bool Position::make_generated_move(const Move& move) noexcept {
    return impl_->position.make_generated_move(move);
}

bool Position::make_generated_move_if_key(const Move& move,
                                          const std::uint64_t expected_key) noexcept {
    return impl_->position.make_generated_move_if_key(move, expected_key);
}

bool Position::make_generated_move_if_key(const Move& move,
                                          const std::uint64_t expected_key,
                                          KeyAndSide& child_out) noexcept {
    return impl_->position.make_generated_move_if_key(move, expected_key, child_out);
}

Position::KeyAndSide Position::position_key_and_side() const noexcept {
    return impl_->position.position_key_and_side();
}

Position::ExchangeSnapshot Position::exchange_snapshot() const noexcept {
    return impl_->position.exchange_snapshot();
}

bool Position::unmake_move() noexcept { return impl_->position.unmake_move(); }

bool Position::make_null_move() noexcept { return impl_->position.make_null_move(); }

bool Position::make_null_move(std::uint64_t& child_key) noexcept {
    return impl_->position.make_null_move(child_key);
}

bool Position::unmake_null_move() noexcept { return impl_->position.unmake_null_move(); }

bool Position::apply_uci(std::string_view uci) {
    const auto move = Move::parse_uci(uci);
    return move.has_value() && impl_->position.apply_legal(*move);
}

bool Position::unapply() { return impl_->position.unapply(); }

bool Position::set_fen(std::string_view fen) {
    NativePosition candidate = impl_->position;
    if (!candidate.set_fen(fen, true)) return false;
    impl_->position = std::move(candidate);
    return true;
}

bool Position::set_fen_unchecked(std::string_view fen) {
    NativePosition candidate = impl_->position;
    if (!candidate.set_fen(fen, false)) return false;
    impl_->position = std::move(candidate);
    return true;
}

std::uint64_t Position::position_key() const noexcept { return impl_->position.position_key(); }

std::uint64_t Position::repetition_history_fingerprint() const noexcept {
    return impl_->position.repetition_history_fingerprint();
}

bool Position::repetition_history_suppressed() const noexcept {
    return impl_->position.repetition_history_suppressed();
}

std::uint64_t Position::pawn_key() const noexcept { return impl_->position.pawn_key(); }

std::size_t Position::piece_count() const noexcept { return impl_->position.piece_count(); }

std::uint64_t Position::piece_bitboard(PieceType type, Color color) const noexcept {
    return impl_->position.piece_bitboard(type, color);
}

const std::array<std::array<std::uint64_t, 7>, 2>& Position::piece_bitboards() const noexcept {
    return impl_->position.state.piece_bitboards;
}

int Position::game_phase() const noexcept {
    const auto& bitboards = impl_->position.state.piece_bitboards;
    constexpr std::size_t kKnightIdx = static_cast<std::size_t>(PieceType::knight);
    constexpr std::size_t kBishopIdx = static_cast<std::size_t>(PieceType::bishop);
    constexpr std::size_t kRookIdx = static_cast<std::size_t>(PieceType::rook);
    constexpr std::size_t kQueenIdx = static_cast<std::size_t>(PieceType::queen);
    const int phase =
        static_cast<int>(std::popcount(bitboards[0][kKnightIdx] | bitboards[1][kKnightIdx] |
                                       bitboards[0][kBishopIdx] | bitboards[1][kBishopIdx])) +
        2 * static_cast<int>(std::popcount(bitboards[0][kRookIdx] | bitboards[1][kRookIdx])) +
        4 * static_cast<int>(std::popcount(bitboards[0][kQueenIdx] | bitboards[1][kQueenIdx]));
    return std::min(phase, 24);
}

std::uint64_t Position::occupied_squares() const noexcept {
    return impl_->position.occupied_squares();
}

bool Position::in_check() const noexcept { return impl_->position.in_check(); }

bool Position::in_check(Color color) const noexcept { return impl_->position.in_check(color); }

std::uint64_t Position::checkers() const noexcept { return impl_->position.checkers(); }

bool Position::has_non_pawn_material(Color color) const noexcept {
    return impl_->position.has_non_pawn_material(color);
}

std::uint8_t Position::castling_rights() const noexcept {
    return impl_->position.castling_rights();
}

Square Position::en_passant_square() const noexcept {
    return impl_->position.en_passant_square();
}

std::size_t Position::repetition_count() const noexcept {
    return impl_->position.repetition_count();
}

bool Position::can_claim_threefold_repetition() const noexcept {
    return impl_->position.can_claim_threefold_repetition();
}

bool Position::can_claim_fifty_move_draw() const noexcept {
    return impl_->position.can_claim_fifty_move_draw();
}

bool Position::is_automatic_fivefold_repetition() const noexcept {
    return impl_->position.is_automatic_fivefold_repetition();
}

bool Position::is_automatic_seventy_five_move_draw() const noexcept {
    return impl_->position.is_automatic_seventy_five_move_draw();
}

bool Position::is_dead_position() const noexcept {
    return impl_->position.is_dead_position();
}

DrawStatus Position::draw_status() const noexcept {
    return impl_->position.draw_status();
}

DrawStatus Position::draw_status(std::size_t repetitions) const noexcept {
    return impl_->position.draw_status(repetitions);
}

Position::NodeEntrySnapshot Position::node_entry_snapshot() const noexcept {
    return impl_->position.node_entry_snapshot();
}

bool Position::is_claimable_draw() const noexcept {
    const DrawStatus status = draw_status();
    return status == DrawStatus::claimable_threefold ||
        status == DrawStatus::claimable_fifty_move;
}

bool Position::is_forced_draw() const noexcept {
    const DrawStatus status = draw_status();
    return status == DrawStatus::dead_position ||
        status == DrawStatus::automatic_fivefold ||
        status == DrawStatus::automatic_seventy_five_move;
}

bool Position::is_repetition_sensitive() const noexcept {
    return impl_->position.is_repetition_sensitive();
}

bool Position::is_draw_by_rule() const noexcept { return impl_->position.is_draw_by_rule(); }

bool Position::is_terminal() const noexcept { return impl_->position.is_terminal(); }

std::uint16_t Position::halfmove_clock() const noexcept { return impl_->position.halfmove_clock(); }

std::uint16_t Position::fullmove_number() const noexcept { return impl_->position.fullmove_number(); }

std::size_t Position::history_size() const noexcept { return impl_->position.history_size(); }

} // namespace koi
