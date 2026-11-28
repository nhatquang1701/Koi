#include "koi/move.hpp"

#include <cstddef>

namespace koi {

std::optional<Square> Square::parse(std::string_view coordinate) noexcept {
    if (coordinate.size() != 2 || coordinate[0] < 'a' || coordinate[0] > 'h' || coordinate[1] < '1' ||
        coordinate[1] > '8') {
        return std::nullopt;
    }

    return Square::from_index(static_cast<std::uint8_t>((coordinate[1] - '1') * 8 + coordinate[0] - 'a'));
}

std::string Square::uci() const {
    if (index() == kInvalid) {
        return {};
    }

    return {file(), rank()};
}

std::optional<Move> Move::parse_uci(std::string_view uci) noexcept {
    if (uci == "0000") {
        return Move::no_move();
    }
    if (uci.size() != 4 && uci.size() != 5) {
        return std::nullopt;
    }

    const auto from = Square::parse(uci.substr(0, 2));
    const auto to = Square::parse(uci.substr(2, 2));
    if (!from || !to) {
        return std::nullopt;
    }

    Promotion promotion = Promotion::none;
    if (uci.size() == 5) {
        switch (uci[4]) {
            case 'n':
                promotion = Promotion::knight;
                break;
            case 'b':
                promotion = Promotion::bishop;
                break;
            case 'r':
                promotion = Promotion::rook;
                break;
            case 'q':
                promotion = Promotion::queen;
                break;
            default:
                return std::nullopt;
        }
    }

    return Move(*from, *to, promotion);
}

std::string_view Move::format_uci(std::array<char, 5>& buffer) const noexcept {
    if (is_no_move()) {
        buffer = {'0', '0', '0', '0', '\0'};
        return std::string_view(buffer.data(), 4);
    }
    if (from().index() == Square::kInvalid || to().index() == Square::kInvalid) {
        return {};
    }

    buffer[0] = from().file();
    buffer[1] = from().rank();
    buffer[2] = to().file();
    buffer[3] = to().rank();
    std::size_t length = 4;
    switch (promotion()) {
        case Promotion::none:
            break;
        case Promotion::knight:
            buffer[length++] = 'n';
            break;
        case Promotion::bishop:
            buffer[length++] = 'b';
            break;
        case Promotion::rook:
            buffer[length++] = 'r';
            break;
        case Promotion::queen:
            buffer[length++] = 'q';
            break;
    }
    return std::string_view(buffer.data(), length);
}

std::string Move::uci() const {
    std::array<char, 5> buffer{};
    return std::string(format_uci(buffer));
}

} // namespace koi
