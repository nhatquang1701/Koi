#include "koi/detail/fen_validation.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace koi::detail {

struct FenLayout {
    std::array<char, 64> squares{};
    int white_king_square = -1;
    int black_king_square = -1;
    int white_kings = 0;
    int black_kings = 0;
    int white_pawns = 0;
    int black_pawns = 0;
    int white_pieces = 0;
    int black_pieces = 0;
};

bool split_fen_fields(std::string_view fen, std::array<std::string_view, 6>& fields) {
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset < fen.size()) {
        while (offset < fen.size() && std::isspace(static_cast<unsigned char>(fen[offset]))) {
            ++offset;
        }
        if (offset == fen.size()) {
            break;
        }
        if (count == fields.size()) {
            return false;
        }

        const std::size_t start = offset;
        while (offset < fen.size() && !std::isspace(static_cast<unsigned char>(fen[offset]))) {
            ++offset;
        }
        fields[count++] = fen.substr(start, offset - start);
    }
    return count == fields.size();
}

bool valid_piece_placement(std::string_view placement, FenLayout& layout) {
    layout = {};
    int rank = 7;
    int width = 0;
    for (char character : placement) {
        if (character == '/') {
            if (width != 8 || rank == 0) {
                return false;
            }
            --rank;
            width = 0;
        } else if (character >= '1' && character <= '8') {
            width += character - '0';
        } else if (std::string_view("PNBRQKpnbrqk").contains(character)) {
            if (width >= 8 || rank < 0) {
                return false;
            }
            if ((character == 'P' || character == 'p') && (rank == 0 || rank == 7)) {
                return false;
            }
            const int square = rank * 8 + width;
            layout.squares[static_cast<std::size_t>(square)] = character;
            if (character == 'K') {
                layout.white_king_square = square;
                ++layout.white_kings;
            } else if (character == 'k') {
                layout.black_king_square = square;
                ++layout.black_kings;
            }
            if (std::isupper(static_cast<unsigned char>(character))) {
                ++layout.white_pieces;
                if (character == 'P') ++layout.white_pawns;
            } else {
                ++layout.black_pieces;
                if (character == 'p') ++layout.black_pawns;
            }
            ++width;
        } else {
            return false;
        }
        if (width > 8) {
            return false;
        }
    }

    if (rank != 0 || width != 8 || layout.white_kings != 1 || layout.black_kings != 1 ||
        layout.white_pawns > 8 || layout.black_pawns > 8 ||
        layout.white_pieces > 16 || layout.black_pieces > 16) {
        return false;
    }

    const int white_rank = layout.white_king_square / 8;
    const int white_file = layout.white_king_square % 8;
    const int black_rank = layout.black_king_square / 8;
    const int black_file = layout.black_king_square % 8;
    const int rank_distance = white_rank - black_rank;
    const int file_distance = white_file - black_file;
    const bool adjacent_kings = rank_distance >= -1 && rank_distance <= 1 && file_distance >= -1 &&
                                file_distance <= 1;
    return !adjacent_kings;
}

bool valid_castling(std::string_view castling, const FenLayout& layout) {
    if (castling == "-") {
        return true;
    }
    if (castling.empty() || castling.size() > 4) {
        return false;
    }
    for (std::size_t index = 0; index < castling.size(); ++index) {
        if (!std::string_view("KQkq").contains(castling[index]) ||
            castling.find(castling[index], index + 1) != std::string_view::npos) {
            return false;
        }
    }

    const auto piece_at = [&layout](int square) { return layout.squares[static_cast<std::size_t>(square)]; };
    return (!castling.contains('K') || (piece_at(4) == 'K' && piece_at(7) == 'R')) &&
           (!castling.contains('Q') || (piece_at(4) == 'K' && piece_at(0) == 'R')) &&
           (!castling.contains('k') || (piece_at(60) == 'k' && piece_at(63) == 'r')) &&
           (!castling.contains('q') || (piece_at(60) == 'k' && piece_at(56) == 'r'));
}

bool valid_en_passant(std::string_view en_passant, std::string_view side_to_move,
                      std::string_view halfmove_clock, const FenLayout& layout) {
    if (en_passant == "-") {
        return true;
    }
    if (en_passant.size() != 2 || en_passant[0] < 'a' || en_passant[0] > 'h' ||
        !((side_to_move == "w" && en_passant[1] == '6') || (side_to_move == "b" && en_passant[1] == '3')) ||
        halfmove_clock != "0") {
        return false;
    }

    const int target_file = en_passant[0] - 'a';
    const int target_rank = en_passant[1] - '1';
    const int target_square = target_rank * 8 + target_file;
    const int pawn_square = (side_to_move == "w" ? target_rank - 1 : target_rank + 1) * 8 + target_file;
    const int origin_square = (side_to_move == "w" ? target_rank + 1 : target_rank - 1) * 8 + target_file;
    const char pawn = side_to_move == "w" ? 'p' : 'P';
    return layout.squares[static_cast<std::size_t>(target_square)] == '\0' &&
           layout.squares[static_cast<std::size_t>(pawn_square)] == pawn &&
           layout.squares[static_cast<std::size_t>(origin_square)] == '\0';
}

bool valid_counter(std::string_view counter, bool allow_zero, std::uint32_t maximum) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(counter.data(), counter.data() + counter.size(), value);
    return error == std::errc{} && end == counter.data() + counter.size() && (allow_zero || value > 0) &&
           value <= maximum;
}

bool valid_fen_syntax(std::string_view fen) {
    std::array<std::string_view, 6> fields{};
    FenLayout layout;
    return split_fen_fields(fen, fields) && valid_piece_placement(fields[0], layout) &&
           (fields[1] == "w" || fields[1] == "b") && valid_castling(fields[2], layout) &&
           valid_en_passant(fields[3], fields[1], fields[4], layout) && valid_counter(fields[4], true, 255) &&
           valid_counter(fields[5], false, 32768);
}

} // namespace koi::detail
