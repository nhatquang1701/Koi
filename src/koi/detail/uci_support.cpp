#include "koi/detail/uci_support.hpp"

#include "koi/detail/uci_option_table.hpp"
#include "koi/classical_evaluator.hpp"
#include "koi/evaluator.hpp"
#include "koi/nnue.hpp"
#include "koi/opening_book.hpp"
#include "koi/perft.hpp"
#include "koi/syzygy_tablebase.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <istream>
#include <limits>
#include <sstream>
#include <utility>

namespace koi::detail {

namespace {

constexpr int kWdlScoreLimit = 1'000;

template <typename T>
std::string debug_optional(const std::optional<T>& value) {
    return value.has_value() ? std::to_string(*value) : "-";
}

bool parse_int64_token(const std::string_view value, std::int64_t& parsed) noexcept {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

bool parse_milliseconds(const std::string_view value, std::chrono::milliseconds& duration) {
    std::uint64_t parsed = 0;
    using Rep = std::chrono::milliseconds::rep;
    if (!parse_uint64(value, parsed) ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
        return false;
    }
    duration = std::chrono::milliseconds{static_cast<Rep>(parsed)};
    return true;
}

std::string format_eval_tokens(const MoveScoreEval& eval) {
    if (eval.mate.has_value()) {
        return "mate " + std::to_string(*eval.mate);
    }
    return "cp " + std::to_string(eval.score_cp);
}

std::optional<ParsedOption> parse_setoption(const std::vector<std::string>& tokens) {
    if (tokens.size() < 2 || tokens[0] != "name") {
        return std::nullopt;
    }
    const auto value = std::find(tokens.begin() + 1, tokens.end(), "value");
    const std::size_t value_index = static_cast<std::size_t>(value - tokens.begin());
    if (value == tokens.end() || value_index == 1) {
        return ParsedOption{join_tokens(tokens, 1, tokens.size()), {}};
    }
    return ParsedOption{join_tokens(tokens, 1, value_index),
                        join_tokens(tokens, value_index + 1, tokens.size())};
}

std::string debug_square(const Square square) {
    return square.index() < Square::kInvalid ? square.uci() : "-";
}

std::string debug_castling(const std::uint8_t rights) {
    std::string result;
    if ((rights & kWhiteKingSideCastling) != 0) result += 'K';
    if ((rights & kWhiteQueenSideCastling) != 0) result += 'Q';
    if ((rights & kBlackKingSideCastling) != 0) result += 'k';
    if ((rights & kBlackQueenSideCastling) != 0) result += 'q';
    return result.empty() ? "-" : result;
}

std::string debug_limits(const SearchLimits& limits) {
    std::ostringstream stream;
    stream << "depth=" << debug_optional(limits.depth)
           << ",nodes=" << debug_optional(limits.nodes)
           << ",movetime_ms=" <<
        (limits.movetime.has_value() ? std::to_string(limits.movetime->count()) : "-")
           << ",wtime_ms=" <<
        (limits.white_clock.has_value() ? std::to_string(limits.white_clock->remaining.count()) : "-")
           << ",btime_ms=" <<
        (limits.black_clock.has_value() ? std::to_string(limits.black_clock->remaining.count()) : "-")
           << ",winc_ms=" <<
        (limits.white_clock.has_value() ? std::to_string(limits.white_clock->increment.count()) : "-")
           << ",binc_ms=" <<
        (limits.black_clock.has_value() ? std::to_string(limits.black_clock->increment.count()) : "-")
           << ",movestogo=" << debug_optional(limits.moves_to_go)
           << ",infinite=" << (limits.infinite ? "true" : "false")
           << ",ponder=" << (limits.ponder ? "true" : "false")
           << ",searchmoves=" << (limits.search_moves_specified ? "true" : "false");
    if (limits.search_moves_specified) {
        std::array<char, 5> move_buf{};
        stream << ",searchmove_list=";
        for (std::size_t index = 0; index < limits.search_moves.size(); ++index) {
            if (index != 0) stream << ',';
            stream << limits.search_moves[index].format_uci(move_buf);
        }
    }
    return stream.str();
}

const char* disposition_name(const CompletionDisposition disposition) noexcept {
    switch (disposition) {
    case CompletionDisposition::emit: return "emit";
    case CompletionDisposition::suppress_stale: return "suppress_stale";
    case CompletionDisposition::fallback: return "fallback";
    case CompletionDisposition::quarantine: return "quarantine";
    }
    return "unknown";
}

std::string debug_hex(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::string hex_bytes(const std::uint8_t* const data, const std::size_t count) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(count * 2U);
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint8_t byte = data[index];
        hex.push_back(kHexDigits[(byte >> 4U) & 0x0fU]);
        hex.push_back(kHexDigits[byte & 0x0fU]);
    }
    return hex;
}

[[nodiscard]] constexpr char ascii_lower(const char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] constexpr bool is_ascii_space(const char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace

BoundedLineRead read_bounded_protocol_line(std::istream& input, std::string& line,
                                           const std::size_t limit, bool& truncated) {
    line.clear();
    truncated = false;
    bool any = false;
    const int eof = std::char_traits<char>::eof();
    for (int character = input.get(); character != eof; character = input.get()) {
        any = true;
        if (character == '\n') {
            return BoundedLineRead::line;
        }
        if (line.size() < limit) {
            line.push_back(static_cast<char>(character));
        } else {
            truncated = true;
        }
    }
    return any ? BoundedLineRead::line : BoundedLineRead::eof;
}

const char* hash_status_name(const HashResizeStatus status) noexcept {
    switch (status) {
    case HashResizeStatus::applied:
        return "applied";
    case HashResizeStatus::reduced:
        return "reduced";
    case HashResizeStatus::unchanged:
        return "unchanged";
    case HashResizeStatus::disabled:
        return "disabled";
    }
    return "unknown";
}

const char* hash_reason_name(const HashResizeReason reason) noexcept {
    switch (reason) {
    case HashResizeReason::none:
        return "none";
    case HashResizeReason::request_clamped:
        return "request_clamped";
    case HashResizeReason::physical_memory_cap:
        return "physical_memory_cap";
    case HashResizeReason::commit_cap:
        return "commit_cap";
    case HashResizeReason::allocation_failed:
        return "allocation_failed";
    case HashResizeReason::startup_unavailable:
        return "startup_unavailable";
    }
    return "unknown";
}

std::vector<std::string> remaining_tokens(std::istream& command) {
    std::vector<std::string> tokens;
    for (std::string token; command >> token;) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

std::string join_tokens(const std::vector<std::string>& tokens, const std::size_t first,
                        const std::size_t last) {
    std::string result;
    for (std::size_t index = first; index < last; ++index) {
        if (!result.empty()) {
            result += ' ';
        }
        result += tokens[index];
    }
    return result;
}

bool equals_ignore_case(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

std::optional<ParsedOption> parse_setoption_for_protocol(const std::vector<std::string>& tokens,
                                                         const bool allow_ucci_shorthand) {
    if (std::optional<ParsedOption> standard = parse_setoption(tokens); standard.has_value()) {
        return standard;
    }
    if (!allow_ucci_shorthand || tokens.size() < 2) {
        return std::nullopt;
    }
    return ParsedOption{tokens[0], join_tokens(tokens, 1, tokens.size())};
}

bool parse_uint64(const std::string_view value, std::uint64_t& parsed) noexcept {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

bool parse_boolean(const std::string_view value, bool& parsed) {
    if (equals_ignore_case(value, "true")) {
        parsed = true;
        return true;
    }
    if (equals_ignore_case(value, "false")) {
        parsed = false;
        return true;
    }
    return false;
}

bool parse_random_seed(const std::string_view value, std::uint32_t& seed) {
    std::uint64_t parsed = 0;
    if (!parse_uint64(value, parsed) || parsed > kMaximumRandomSeed) {
        return false;
    }
    seed = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_xboard_seconds(const std::string_view value, std::chrono::milliseconds& duration) {
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    const std::string buffer(value);
    const double seconds = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str() || *end != '\0' || !std::isfinite(seconds) ||
        seconds < 0.0 || seconds > 86'400'000.0) {
        return false;
    }
    duration = std::chrono::milliseconds{static_cast<std::int64_t>(seconds * 1000.0 + 0.5)};
    return true;
}

bool parse_xboard_level_time(const std::string_view value, std::chrono::milliseconds& duration) {
    const std::size_t colon = value.find(':');
    if (colon == std::string_view::npos) {
        std::uint64_t minutes = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), minutes);
        if (error != std::errc{} || end != value.data() + value.size() || minutes > 1440) {
            return false;
        }
        duration = std::chrono::minutes{static_cast<std::int64_t>(minutes)};
        return true;
    }
    std::uint64_t minutes = 0;
    std::uint64_t seconds = 0;
    const std::string_view min_part = value.substr(0, colon);
    const std::string_view sec_part = value.substr(colon + 1);
    const auto [mend, merr] =
        std::from_chars(min_part.data(), min_part.data() + min_part.size(), minutes);
    const auto [send, serr] =
        std::from_chars(sec_part.data(), sec_part.data() + sec_part.size(), seconds);
    if (merr != std::errc{} || mend != min_part.data() + min_part.size() ||
        serr != std::errc{} || send != sec_part.data() + sec_part.size() ||
        minutes > 1440 || seconds >= 60) {
        return false;
    }
    duration = std::chrono::minutes{static_cast<std::int64_t>(minutes)} +
               std::chrono::seconds{static_cast<std::int64_t>(seconds)};
    return true;
}

int cecp_mate_score(const int mate_in_moves) noexcept {
    if (mate_in_moves > 0) {
        return 100'000 - (2 * mate_in_moves - 1);
    }
    return -100'000 + (-2 * mate_in_moves);
}

std::string xboard_terminal_result_line(const GameState& state) {
    if (!state.has_legal_move()) {
        if (state.in_check()) {
            return state.side_to_move() == Color::white ? "0-1 {Black mates}" : "1-0 {White mates}";
        }
        return "1/2-1/2 {Stalemate}";
    }
    switch (state.draw_status()) {
    case DrawStatus::automatic_fivefold:
    case DrawStatus::claimable_threefold:
        return "1/2-1/2 {Draw by repetition}";
    case DrawStatus::automatic_seventy_five_move:
    case DrawStatus::claimable_fifty_move:
        return "1/2-1/2 {50-move rule}";
    case DrawStatus::dead_position:
        return "1/2-1/2 {Insufficient material}";
    case DrawStatus::none:
        return "1/2-1/2 {Draw}";
    }
    return "1/2-1/2 {Draw}";
}

std::uint32_t root_ply(const GameState& state) {
    const std::uint32_t completed_fullmoves = static_cast<std::uint32_t>(state.fullmove_number() - 1);
    return completed_fullmoves * 2 + (state.side_to_move() == Color::black ? 1U : 0U);
}

Wdl score_to_wdl(const SearchInfo& info) noexcept {
    if (info.exact_wdl.has_value()) {
        const std::array<int, 3>& triplet = *info.exact_wdl;
        return Wdl{triplet[0], triplet[1], triplet[2]};
    }
    if (info.mate.has_value()) {
        return *info.mate > 0 ? Wdl{1'000, 0, 0} : Wdl{0, 0, 1'000};
    }
    const int score = std::clamp(info.score_cp, -kWdlScoreLimit, kWdlScoreLimit);
    const int draw = std::max(0, 300 - std::abs(score) / 4);
    const int decisive = 1'000 - draw;
    const int win = decisive * (score + kWdlScoreLimit) / (2 * kWdlScoreLimit);
    return Wdl{win, draw, decisive - win};
}

std::string debug_quoted(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '\\':
        case '"':
            result.push_back('\\');
            result.push_back(character);
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                std::ostringstream escaped;
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(static_cast<unsigned char>(character));
                result += escaped.str();
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::uint64_t legal_move_digest(const std::span<const Move> legal_moves) {
    const auto sort_and_hash = [](std::span<Move> sorted) noexcept -> std::uint64_t {
        std::sort(sorted.begin(), sorted.end(), [](const Move& left, const Move& right) noexcept {
            std::array<char, 5> left_buf{};
            std::array<char, 5> right_buf{};
            return left.format_uci(left_buf) < right.format_uci(right_buf);
        });

        std::uint64_t digest = 1469598103934665603ULL;
        std::array<char, 5> buffer{};
        for (const Move& move : sorted) {
            for (const char character : move.format_uci(buffer)) {
                digest ^= static_cast<unsigned char>(character);
                digest *= 1099511628211ULL;
            }
            digest ^= static_cast<unsigned char>('\n');
            digest *= 1099511628211ULL;
        }
        return digest;
    };

    if (legal_moves.size() <= kMaximumLegalMoves) {
        std::array<Move, kMaximumLegalMoves> stack_moves{};
        std::copy(legal_moves.begin(), legal_moves.end(), stack_moves.begin());
        return sort_and_hash(std::span<Move>(stack_moves.data(), legal_moves.size()));
    }

    std::vector<Move> heap_moves(legal_moves.begin(), legal_moves.end());
    return sort_and_hash(heap_moves);
}

std::vector<Move> legal_pv_prefix(const GameState& root, const std::vector<Move>& pv) {
    GameState position = root;
    std::vector<Move> prefix;
    prefix.reserve(pv.size());
    for (const Move& move : pv) {
        if (!position.is_legal(move) || !position.make_move(move)) {
            break;
        }
        prefix.push_back(move);
    }
    return prefix;
}

bool parse_annotate_limits(const std::vector<std::string>& tokens, const std::size_t first_index,
                           SearchLimits& limits, std::string& error) {
    for (std::size_t index = first_index; index < tokens.size();) {
        const std::string& token = tokens[index];
        if (token == "depth") {
            std::uint64_t parsed = 0;
            if (index + 1 >= tokens.size() || !parse_uint64(tokens[index + 1], parsed) ||
                parsed == 0 ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                error = "annotation depth must be positive";
                return false;
            }
            limits.depth = static_cast<int>(parsed);
            index += 2;
        } else if (token == "movetime") {
            std::chrono::milliseconds parsed{};
            if (index + 1 >= tokens.size() || !parse_milliseconds(tokens[index + 1], parsed)) {
                error = "annotation movetime must be non-negative";
                return false;
            }
            limits.movetime = parsed;
            index += 2;
        } else if (token == "nodes") {
            std::uint64_t parsed = 0;
            if (index + 1 >= tokens.size() || !parse_uint64(tokens[index + 1], parsed) ||
                parsed == 0) {
                error = "annotation nodes must be positive";
                return false;
            }
            limits.nodes = parsed;
            index += 2;
        } else {
            error = "unsupported annotation limit: " + token;
            return false;
        }
    }
    return true;
}

std::string format_ascii_board(const GameState& position) {
    std::array<Piece, 64> board{};
    position.copy_board_to(board);
    const std::string fen = position.fen();
    const std::uint64_t key = position.position_key();
    const bool white_to_move = (position.side_to_move() == Color::white);
    const bool checked = position.in_check();

    auto piece_symbol = [](const Piece piece) noexcept -> char {
        char symbol = ' ';
        switch (piece.type) {
        case PieceType::pawn:   symbol = 'P'; break;
        case PieceType::knight: symbol = 'N'; break;
        case PieceType::bishop: symbol = 'B'; break;
        case PieceType::rook:   symbol = 'R'; break;
        case PieceType::queen:  symbol = 'Q'; break;
        case PieceType::king:   symbol = 'K'; break;
        case PieceType::none:   return ' ';
        }
        return piece.color == Color::white ? symbol : static_cast<char>(symbol - 'A' + 'a');
    };

    std::ostringstream buffer;
    buffer << "\n +---+---+---+---+---+---+---+---+\n";
    for (int rank = 7; rank >= 0; --rank) {
        buffer << " |";
        for (int file = 0; file < 8; ++file) {
            const Piece piece = board[static_cast<std::size_t>(rank * 8 + file)];
            buffer << ' ' << piece_symbol(piece) << " |";
        }
        buffer << ' ' << (rank + 1) << "\n +---+---+---+---+---+---+---+---+\n";
    }
    const std::size_t legal_count = position.legal_moves_into({});
    std::string_view state_label = "ongoing";
    if (legal_count == 0) {
        state_label = checked
            ? (white_to_move ? "checkmate (black wins)" : "checkmate (white wins)")
            : "stalemate";
    } else {
        switch (position.draw_status()) {
        case DrawStatus::automatic_fivefold:
            state_label = "draw (fivefold repetition)";
            break;
        case DrawStatus::claimable_threefold:
            state_label = "draw (threefold repetition)";
            break;
        case DrawStatus::automatic_seventy_five_move:
            state_label = "draw (75-move rule)";
            break;
        case DrawStatus::claimable_fifty_move:
            state_label = "draw (50-move rule)";
            break;
        case DrawStatus::dead_position:
            state_label = "draw (insufficient material)";
            break;
        case DrawStatus::none:
            state_label = "ongoing";
            break;
        }
    }
    const std::uint64_t checkers_mask = position.checkers();
    buffer << "   a   b   c   d   e   f   g   h\n\n"
           << "Fen   : " << fen << '\n'
           << "Key   : " << debug_hex(key) << '\n'
           << "Side  : " << (white_to_move ? "white" : "black") << '\n'
           << "Check : " << (checked ? "yes" : "no");
    if (checkers_mask != 0) {
        buffer << " (";
        bool first_checker = true;
        std::uint64_t remaining = checkers_mask;
        while (remaining != 0) {
            const auto square = static_cast<std::uint8_t>(std::countr_zero(remaining));
            remaining &= remaining - 1;
            if (!first_checker) {
                buffer << ' ';
            }
            first_checker = false;
            buffer << Square::from_index(square).uci();
        }
        buffer << ')';
    }
    buffer << '\n'
           << "Moves : " << legal_count << '\n'
           << "State : " << state_label << '\n';
    return buffer.str();
}

std::string format_key_info(const GameState& position) {
    std::ostringstream buffer;
    buffer << "Position key : " << debug_hex(position.position_key()) << '\n'
           << "Pawn key     : " << debug_hex(position.pawn_key()) << '\n'
           << "Polyglot key : " << debug_hex(position.polyglot_key()) << '\n';
    return buffer.str();
}

namespace {

std::string format_legal_moves_impl(const GameState& position, const bool san) {
    std::array<Move, kMaximumLegalMoves> raw_moves{};
    const std::size_t move_count =
        std::min(position.legal_moves_into(raw_moves), raw_moves.size());
    std::vector<std::string> moves;
    moves.reserve(move_count);
    for (std::size_t index = 0; index < move_count; ++index) {
        if (san) {
            const std::optional<std::string> text = move_to_san(position, raw_moves[index]);
            moves.push_back(text.has_value() ? *text : raw_moves[index].uci());
        } else {
            moves.push_back(raw_moves[index].uci());
        }
    }
    std::sort(moves.begin(), moves.end());
    std::ostringstream buffer;
    buffer << "Legal moves (" << moves.size() << "):";
    if (moves.empty()) {
        buffer << " (none)";
    } else {
        for (const std::string& move : moves) {
            buffer << ' ' << move;
        }
    }
    buffer << '\n';
    return buffer.str();
}

} // namespace

std::string format_legal_moves(const GameState& position) {
    return format_legal_moves_impl(position, false);
}

std::string format_legal_moves_san(const GameState& position) {
    return format_legal_moves_impl(position, true);
}

std::string format_book_entries(const std::string_view book_name,
                                    const std::vector<BookChoice>& entries) {
    std::ostringstream buffer;
    if (entries.empty()) {
        buffer << "Opening book : " << book_name << " (no moves for position)\n";
        return buffer.str();
    }
    std::uint64_t total_weight = 0;
    for (const BookChoice& entry : entries) {
        total_weight += entry.weight;
    }
    buffer << "Opening book : " << book_name << " (" << entries.size()
           << " moves, total weight " << total_weight << ")\n"
           << std::fixed << std::setprecision(1);
    std::array<char, 5> move_buf{};
    for (const BookChoice& entry : entries) {
        const double pct = total_weight > 0
            ? (100.0 * static_cast<double>(entry.weight) / static_cast<double>(total_weight))
            : 0.0;
        buffer << "  " << entry.move.format_uci(move_buf) << "  weight=" << entry.weight
               << " (" << pct << "%)\n";
    }
    return buffer.str();
}

std::string format_syzygy_probe(const SyzygyTablebase* const syzygy, const GameState& position) {
    std::ostringstream buffer;
    if (syzygy == nullptr || !syzygy->enabled()) {
        buffer << "Syzygy : unavailable\n";
    } else if (!syzygy->probe_eligible(position)) {
        buffer << "Syzygy : loaded (max " << syzygy->large_table_limit()
               << " pieces; position has " << position.piece_count()
               << ", not eligible)\n";
    } else if (const std::optional<SyzygyRootResult> root = syzygy->probe_root(position);
               !root.has_value()) {
        buffer << "Syzygy : loaded (max " << syzygy->large_table_limit()
               << " pieces; no hit)\n";
    } else {
        const char* wdl_name = "draw";
        switch (root->wdl) {
        case SyzygyWdl::win:
            wdl_name = "win";
            break;
        case SyzygyWdl::cursed_win:
            wdl_name = "cursed win";
            break;
        case SyzygyWdl::draw:
            wdl_name = "draw";
            break;
        case SyzygyWdl::blessed_loss:
            wdl_name = "blessed loss";
            break;
        case SyzygyWdl::loss:
            wdl_name = "loss";
            break;
        }
        buffer << "Syzygy : " << wdl_name << " (" << std::showpos << root->score.score_cp
               << " cp) | moves:";
        if (root->moves.empty()) {
            buffer << " (none)";
        } else {
            std::array<char, 5> move_buf{};
            for (const Move& move : root->moves) {
                buffer << ' ' << move.format_uci(move_buf);
            }
        }
        buffer << '\n';
    }
    return buffer.str();
}

std::string format_eval_summary(const GameState& position,
                                const Evaluator* const evaluator,
                                const bool hce_active) {
    std::ostringstream buffer;
    if (evaluator == nullptr) {
        buffer << "Evaluator      : unavailable\n";
        return buffer.str();
    }
    const int score_stm = evaluator->evaluate(position, position.side_to_move());
    const int score_white =
        (position.side_to_move() == Color::white) ? score_stm : -score_stm;
    auto format_score = [](const int cp) {
        std::ostringstream out;
        out << std::showpos << std::fixed << std::setprecision(2)
            << (static_cast<double>(cp) / 100.0) << " (" << cp << " cp)";
        return out.str();
    };
    SearchInfo eval_info;
    eval_info.score_cp = score_stm;
    const Wdl wdl = score_to_wdl(eval_info);
    const NnueEvaluator* const nnue =
        hce_active ? nullptr : dynamic_cast<const NnueEvaluator*>(evaluator);
    buffer << "Evaluator      : ";
    if (hce_active) {
        buffer << "HCE (classical tapered)";
    } else if (nnue != nullptr && nnue->network() != nullptr) {
        const NnueManifest& m = nnue->network()->manifest;
        const bool dual =
            (m.version == kKoiNnueHalfkaHmV9FormatVersion) ||
            (m.version == kKoiNnueHalfkaKingBucketV1FormatVersion && m.layer_sizes[3] == 2U);
        buffer << "NNUE (v" << m.version << ' ' << m.feature_set << ", "
               << m.layer_sizes[0] << "->" << m.layer_sizes[1]
               << (dual ? "x2->" : "->") << m.layer_sizes[2] << ')';
    } else {
        buffer << "NNUE";
    }
    buffer << '\n'
           << "Side to move   : " << format_score(score_stm) << '\n'
           << "White relative : " << format_score(score_white) << '\n'
           << "WDL (stm)      : " << std::noshowpos << std::fixed << std::setprecision(1)
           << (static_cast<double>(wdl.win) / 10.0) << "% W / "
           << (static_cast<double>(wdl.draw) / 10.0) << "% D / "
           << (static_cast<double>(wdl.loss) / 10.0) << "% L\n"
           << "Game phase     : " << position.game_phase() << "/24 (pieces: "
           << position.piece_count() << ")\n";
    if (nnue != nullptr && nnue->network() != nullptr) {
        const NnueNetwork& net = *nnue->network();
        const NnueManifest& m = net.manifest;
        buffer << "Network SHA256 : " << hex_bytes(m.network_sha256.data(), 6) << '\n';
        if (m.layer_sizes[2] == kKoiNnueOutputBucketCount) {
            const std::size_t missing =
                32U - std::min<std::size_t>(32U, position.piece_count());
            const std::size_t bucket = std::min<std::size_t>(7U, missing / 4U);
            buffer << "Output bucket  : " << bucket << " (of 8)\n";
        }
        buffer << "Quantization   : " << m.quantization;
        if (m.version == kKoiNnueHalfkaHmV9FormatVersion) {
            buffer << " (QA=" << static_cast<unsigned>(net.qa)
                   << ", QB=" << static_cast<unsigned>(net.qb)
                   << ", scale=" << net.eval_scale << ')';
        }
        buffer << '\n';
    }
    if (const auto* hce = dynamic_cast<const ClassicalEvaluator*>(evaluator); hce != nullptr) {
        const ClassicalEvalTrace tr = hce->trace(position);
        const int phase = std::clamp(tr.phase, 0, 24);
        auto write_row = [&buffer, phase](const char* label, const ClassicalScore s) {
            const int total_cp = (s.mg * phase + s.eg * (24 - phase)) / 24;
            buffer << std::left << std::setw(15) << label << "| "
                   << std::right << std::showpos << std::fixed << std::setprecision(2)
                   << std::setw(7) << (static_cast<double>(s.mg) / 100.0) << " | "
                   << std::setw(7) << (static_cast<double>(s.eg) / 100.0) << " | "
                   << std::setw(6) << (static_cast<double>(total_cp) / 100.0) << '\n';
        };
        buffer << "\nTerm           |      MG |      EG |   Total\n"
               << "---------------+---------+---------+--------\n";
        write_row("Material", tr.material);
        write_row("PST", tr.pst);
        write_row("Mobility", tr.mobility);
        write_row("Pawns", tr.pawns);
        write_row("King safety", tr.king_safety);
        write_row("Threats", tr.threats);
        write_row("Pieces", tr.piece_terms);
        write_row("Space & dev", tr.space_dev);
        write_row("Tempo", tr.tempo);
        buffer << "---------------+---------+---------+--------\n"
               << std::left << std::setw(15) << "Total (White)" << "| "
               << std::right << std::showpos << std::fixed << std::setprecision(2)
               << std::setw(7) << (static_cast<double>(tr.total.mg) / 100.0) << " | "
               << std::setw(7) << (static_cast<double>(tr.total.eg) / 100.0) << " | "
               << std::setw(6) << (static_cast<double>(tr.total_white_cp) / 100.0) << '\n'
               << std::noshowpos;
        if (tr.insufficient_material) {
            buffer << "Note           : insufficient material draw (score 0 cp)\n";
        } else if (tr.opposite_bishop_draw) {
            buffer << "Note           : opposite-colored bishops damping applied (1/4)\n";
        }
    }
    return buffer.str();
}

std::string format_interactive_help() {
    return
        "Koi interactive commands:\n"
        "  uci                                 Enter UCI mode and print options\n"
        "  isready                             Synchronize with the engine\n"
        "  position [startpos|fen ...] [moves] Set the board position\n"
        "  go [depth|movetime|nodes|infinite]  Start searching\n"
        "  stop                                Stop the active search\n"
        "  wait                                Wait for a finite search or perft to finish\n"
        "  d                                   Display ASCII board, FEN, key, and move count\n"
        "  fen                                 Print the current position FEN\n"
        "  epd                                 Print standard EPD (with hmvc and fmvn) of the current position\n"
        "  key                                 Print Zobrist position, pawn, and Polyglot hash keys\n"
        "  moves [san]                         List sorted legal UCI (or FIDE SAN) moves from current position\n"
        "  book                                Probe active Polyglot opening book entries\n"
        "  tb                                  Probe active Syzygy tablebases for root WDL/moves\n"
        "  eval                                Show active evaluator, score, WDL, and phase\n"
        "  nnue                                Show active loaded NNUE metadata and SHA-256\n"
        "  flip                                Flip side to move when legal\n"
        "  makemove <move...>                  Play one or more legal UCI/SAN moves on the current board\n"
        "  undo [plies]                        Step back N plies (default 1) in position history\n"
        "  history                             Show position origin, move history, and UCI command\n"
        "  pgn                                 Export current position and move history as FIDE PGN\n"
        "  annotate <move|game> [limits]       Classify a UCI/SAN move or game history with accuracy & CPL\n"
        "  see <move>                          Show static exchange evaluation for a legal UCI/SAN move\n"
        "  perft <depth|suite>                 Run interruptible perft divide (1..10) or 6-position CPW suite\n"
        "  bench [depth [hash [threads]]]      Run OpenBench-compatible benchmark\n"
        "  speedtest [threads [hash [secs]]]   Run timed NPS throughput benchmark\n"
        "  options                             Show live active UCI option values\n"
        "  compiler                            Show compiler, C++ standard, and ISA\n"
        "  help                                Show this command summary\n"
        "  quit                                Exit the engine\n";
}

std::string format_options_summary(const LiveOptionsSnapshot& snapshot) {
    const auto bool_str = [](const bool value) noexcept -> const char* {
        return value ? "true" : "false";
    };
    std::ostringstream buffer;
    buffer << "Protocol        : " << snapshot.protocol_name << '\n'
           << "Evaluator       : " << snapshot.evaluator_name << '\n'
           << "EvalFile        : "
           << (snapshot.eval_file.empty() ? "<autodiscovery>" : snapshot.eval_file) << '\n'
           << "Hash            : " << snapshot.hash_mb << " MB\n"
           << "Threads         : " << snapshot.threads << '\n'
           << "MultiPV         : " << snapshot.multi_pv << '\n'
           << "Ponder          : " << bool_str(snapshot.ponder) << '\n'
           << "UCI_AnalyseMode : " << bool_str(snapshot.analyse_mode) << '\n'
           << "UCI_ShowWDL     : " << bool_str(snapshot.show_wdl) << '\n'
           << "Contempt        : " << snapshot.contempt_cp << " cp\n"
           << "Move Overhead   : " << snapshot.move_overhead_ms
           << " ms (dynamic: " << bool_str(snapshot.dynamic_overhead)
           << ", observed: " << snapshot.observed_gui_overhead_ms << " ms)\n"
           << "Slow Mover      : " << snapshot.slow_mover_percent << " %\n"
           << "RandomSeed      : " << snapshot.random_seed << '\n'
           << "OwnBook         : " << bool_str(snapshot.own_book)
           << " (explicit: " << bool_str(snapshot.own_book_explicit)
           << ", available: " << bool_str(snapshot.book_available)
           << ", file: " << snapshot.book_file
           << ", depth: " << static_cast<unsigned>(snapshot.book_depth)
           << ", random: " << bool_str(snapshot.book_random)
           << ", safety: " << bool_str(snapshot.book_safety) << '/'
           << static_cast<unsigned>(snapshot.book_safety_depth) << ")\n"
           << "SyzygyPath      : "
           << (snapshot.syzygy_path.empty() ? "<none>" : snapshot.syzygy_path)
           << " (auto: " << bool_str(snapshot.syzygy_auto_detected)
           << ", depth: " << static_cast<unsigned>(snapshot.syzygy_probe_depth)
           << ", limit: " << static_cast<unsigned>(snapshot.syzygy_probe_limit)
           << ", interior: " << static_cast<unsigned>(snapshot.syzygy_interior_depth)
           << ", 50-move: " << bool_str(snapshot.syzygy_50_move_rule) << ")\n";
    return buffer.str();
}

std::string format_nnue_info(const std::string_view display_path, const NnueNetwork& network) {
    const NnueManifest& manifest = network.manifest;
    std::ostringstream buffer;
    buffer << "NNUE file      : " << display_path << '\n'
           << "Format version : v" << manifest.version << '\n'
           << "Feature set    : " << manifest.feature_set << '\n'
           << "Architecture   : " << manifest.layer_sizes[0] << " -> "
           << manifest.layer_sizes[1];
    if (manifest.version == kKoiNnueHalfkaHmV9FormatVersion ||
        manifest.version == kKoiNnueHalfkaKingBucketV1FormatVersion ||
        (manifest.version == kKoiNnueHalfkaThreatV8FormatVersion &&
         manifest.layer_sizes[0] == kKoiNnueHalfkaKingBucketV1FeatureCount)) {
        buffer << " x 2 -> " << manifest.layer_sizes[2];
    } else if (manifest.version == kKoiNnueHalfkaThreatV5FormatVersion ||
               manifest.version == kKoiNnueHalfkaThreatV6FormatVersion ||
               manifest.version == kKoiNnueHalfkaThreatV8FormatVersion) {
        buffer << " x 2 -> " << manifest.layer_sizes[2] << " -> " << manifest.layer_sizes[3];
    } else {
        buffer << " -> " << manifest.layer_sizes[2] << " -> " << manifest.layer_sizes[3];
    }
    buffer << '\n' << "Quantization   : " << manifest.quantization;
    if (manifest.version == kKoiNnueHalfkaHmV9FormatVersion) {
        buffer << " (QA=" << static_cast<unsigned>(network.qa)
               << ", QB=" << static_cast<unsigned>(network.qb)
               << ", scale=" << network.eval_scale << ')';
    } else if (network.hidden_shift != 0 || network.bottleneck_shift != 0 ||
               network.output_shift != 0 || network.l1_shift != 0 ||
               network.l1_diff_shift != 0) {
        buffer << " (shifts:"
               << " h=" << static_cast<unsigned>(network.hidden_shift)
               << " b=" << static_cast<unsigned>(network.bottleneck_shift)
               << " o=" << static_cast<unsigned>(network.output_shift);
        if (network.l1_shift != 0 || network.l1_diff_shift != 0) {
            buffer << " l1=" << static_cast<unsigned>(network.l1_shift)
                   << " diff=" << static_cast<unsigned>(network.l1_diff_shift);
        }
        buffer << ')';
    }
    buffer << '\n'
           << "Payload bytes  : " << manifest.payload_size << '\n'
           << "Payload SHA256 : "
           << hex_bytes(manifest.network_sha256.data(), manifest.network_sha256.size()) << '\n'
           << "Status         : valid (checksum verified)\n";
    return buffer.str();
}

std::string format_compiler_info(const std::string_view version, const std::string_view commit) {
    std::ostringstream buffer;
    buffer << "Koi " << version << " (" << commit << ")\n";
    buffer << "Compiled by    : ";
#if defined(_MSC_VER)
    buffer << "MSVC " << _MSC_VER;
#elif defined(__apple_build_version__)
    buffer << "AppleClang " << __clang_major__ << '.' << __clang_minor__ << '.'
           << __clang_patchlevel__;
#elif defined(__clang__)
    buffer << "Clang " << __clang_major__ << '.' << __clang_minor__ << '.'
           << __clang_patchlevel__;
#elif defined(__GNUC__)
    buffer << "GCC " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#else
    buffer << "Unknown";
#endif
    buffer << "\nC++ standard   : C++26\nArchitecture   : ";
#if defined(__x86_64__) || defined(_M_X64)
    buffer << "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    buffer << "arm64";
#else
    buffer << "64-bit";
#endif
    buffer << "\nSIMD / ISA     : ";
#if defined(KOI_CPU_REQUIRES_AVX512) || defined(__AVX512F__)
    buffer << "AVX-512";
#elif defined(KOI_CPU_REQUIRES_AVX2) || defined(__AVX2__)
    buffer << "AVX2";
#elif defined(__ARM_NEON)
    buffer << "NEON";
#else
    buffer << "baseline";
#endif
#if defined(KOI_PEXT_ATTACKS) || defined(__BMI2__)
    buffer << " + BMI2 PEXT";
#endif
    buffer << '\n';
    return buffer.str();
}

SearchLimits parse_go_limits(const std::string_view arguments,
                             const std::chrono::milliseconds bare_go_fallback) {
    std::istringstream command{std::string(arguments)};
    const std::vector<std::string> tokens = remaining_tokens(command);
    SearchLimits limits;
    std::optional<std::chrono::milliseconds> white_time;
    std::optional<std::chrono::milliseconds> black_time;
    std::optional<std::chrono::milliseconds> white_increment;
    std::optional<std::chrono::milliseconds> black_increment;

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (token == "infinite") {
            limits.infinite = true;
            continue;
        }
        if (token == "ponder") {
            limits.ponder = true;
            continue;
        }
        if (token == "searchmoves") {
            limits.search_moves_specified = true;
            ++index;
            while (index < tokens.size()) {
                const auto move = Move::parse_uci(tokens[index]);
                if (!move.has_value()) {
                    break;
                }
                limits.search_moves.push_back(*move);
                ++index;
            }
            if (index < tokens.size()) {
                --index;
            }
            continue;
        }
        if (index + 1 >= tokens.size()) {
            continue;
        }

        const std::string& value = tokens[index + 1];
        if (token == "depth") {
            std::uint64_t parsed = 0;
            if (parse_uint64(value, parsed) && parsed > 0 &&
                parsed <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                limits.depth = static_cast<int>(parsed);
            }
            ++index;
        } else if (token == "mate") {
            std::uint64_t parsed = 0;
            if (parse_uint64(value, parsed) && parsed > 0 && parsed <= 64) {
                limits.mate = static_cast<int>(parsed);
            }
            ++index;
        } else if (token == "perft") {
            std::uint64_t parsed = 0;
            // Perft is a debugging command; the cap keeps a typo from asking
            // for an astronomically long walk.
            if (parse_uint64(value, parsed) && parsed > 0 && parsed <= 10) {
                limits.perft = static_cast<int>(parsed);
            }
            ++index;
        } else if (token == "nodes") {
            std::uint64_t parsed = 0;
            if (parse_uint64(value, parsed)) {
                limits.nodes = parsed;
            }
            ++index;
        } else if (token == "movetime") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                limits.movetime = parsed;
            }
            ++index;
        } else if (token == "wtime") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                white_time = parsed;
            }
            ++index;
        } else if (token == "btime") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                black_time = parsed;
            }
            ++index;
        } else if (token == "winc") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                white_increment = parsed;
            }
            ++index;
        } else if (token == "binc") {
            std::chrono::milliseconds parsed;
            if (parse_milliseconds(value, parsed)) {
                black_increment = parsed;
            }
            ++index;
        } else if (token == "movestogo") {
            std::uint64_t parsed = 0;
            if (parse_uint64(value, parsed) && parsed > 0 &&
                parsed <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                limits.moves_to_go = static_cast<std::uint32_t>(parsed);
            }
            ++index;
        }
    }

    if (white_time.has_value()) {
        limits.white_clock = ClockLimit{white_time.value_or(std::chrono::milliseconds::zero()),
                                        white_increment.value_or(std::chrono::milliseconds::zero())};
    }
    if (black_time.has_value()) {
        limits.black_clock = ClockLimit{black_time.value_or(std::chrono::milliseconds::zero()),
                                        black_increment.value_or(std::chrono::milliseconds::zero())};
    }

    if (limits.mate.has_value()) {
        // A UCI go command may provide several limits; the first reached ends
        // the search. Keep the stricter of explicit depth and the mate proof
        // horizon so `mate N` is not discarded when `depth` is also present.
        const int mate_depth = 2 * *limits.mate - 1;
        if (!limits.depth.has_value() || mate_depth < *limits.depth) {
            limits.depth = mate_depth;
        }
    }

    const bool has_usable_limit = limits.depth.has_value() || limits.nodes.has_value() ||
        limits.movetime.has_value() || limits.white_clock.has_value() ||
        limits.black_clock.has_value() || limits.infinite || limits.ponder ||
        limits.mate.has_value() || limits.perft.has_value();
    if (!has_usable_limit) {
        limits.movetime = bare_go_fallback;
    }
    return limits;
}

std::uint32_t derive_network_overhead_ms(const std::int64_t previous_remaining_ms,
                                         const std::int64_t increment_ms,
                                         const std::int64_t elapsed_ms,
                                         const std::int64_t current_remaining_ms,
                                         const std::uint32_t previous_estimate_ms) noexcept {
    if (previous_remaining_ms < 0 || increment_ms < 0 || elapsed_ms < 0 ||
        current_remaining_ms < 0) {
        return previous_estimate_ms;
    }
    const std::int64_t expected = previous_remaining_ms + increment_ms - elapsed_ms;
    const std::int64_t excess = expected - current_remaining_ms;
    // Below the noise floor the GUI clock rounds, and a negative delta means the
    // GUI clock is newer than Koi's elapsed estimate (or the increment was
    // applied differently): decay rather than permanently slowing the engine.
    constexpr std::int64_t kNoiseFloorMs = 25;
    constexpr std::int64_t kMaximumEstimateMs = 500;
    if (excess >= kNoiseFloorMs) {
        const auto candidate = static_cast<std::uint32_t>(
            std::min<std::int64_t>(excess, kMaximumEstimateMs));
        return std::max(previous_estimate_ms, candidate);
    }
    if (excess <= 0) {
        return previous_estimate_ms - previous_estimate_ms / 8U;
    }
    return previous_estimate_ms;
}

std::string format_annotate_move_line(const MoveAnnotationResult& result) {
    std::ostringstream line;
    std::array<char, 5> played_buf{};
    std::array<char, 5> best_buf{};
    line << std::fixed;
    line << "annotate move " << result.played_move.format_uci(played_buf)
         << " ply " << result.ply
         << " side " << (result.side_to_move == Color::white ? "white" : "black")
         << " best " << result.best_move.format_uci(best_buf)
         << " classification " << move_annotation_name(result.annotation)
         << " cpl " << result.centipawn_loss
         << " accuracy " << std::setprecision(1) << result.accuracy_percent
         << " wp_before " << std::setprecision(3) << result.win_prob_before
         << " wp_after " << std::setprecision(3) << result.win_prob_after
         << " eval " << format_eval_tokens(result.played_eval)
         << " best_eval " << format_eval_tokens(result.best_eval)
         << " depth " << result.depth;
    if (const std::string_view glyph = move_annotation_glyph(result.annotation); !glyph.empty()) {
        line << " glyph " << glyph;
    }
    if (const int nag = move_annotation_nag(result.annotation); nag > 0) {
        line << " nag " << nag;
    }
    if (!result.best_pv.empty()) {
        line << " pv";
        for (const Move& pv_move : result.best_pv) {
            line << ' ' << pv_move.format_uci(best_buf);
        }
    }
    return line.str();
}

std::string format_annotate_summary_line(const char* side_name,
                                         const PlayerAnnotationSummary& summary) {
    std::ostringstream line;
    line << std::fixed;
    line << "annotate summary " << side_name
         << " moves " << summary.move_count
         << " accuracy " << std::setprecision(1) << summary.average_accuracy
         << " avg_cpl " << std::setprecision(1) << summary.average_cpl
         << " brilliant " << summary.brilliant
         << " great " << summary.great
         << " best " << summary.best
         << " excellent " << summary.excellent
         << " good " << summary.good
         << " book " << summary.book
         << " forced " << summary.forced
         << " inaccuracy " << summary.inaccuracy
         << " mistake " << summary.mistake
         << " miss " << summary.miss
         << " blunder " << summary.blunder;
    return line.str();
}

std::optional<std::string> flip_position_fen(const std::string_view fen) {
    std::istringstream fen_stream{std::string(fen)};
    std::vector<std::string> fields;
    for (std::string token; fen_stream >> token;) {
        fields.push_back(std::move(token));
    }
    if (fields.size() != 6 || (fields[1] != "w" && fields[1] != "b")) {
        return std::nullopt;
    }
    fields[1] = (fields[1] == "w") ? "b" : "w";
    fields[3] = "-";
    return join_tokens(fields, 0, fields.size());
}

std::string translate_ucci_go_arguments(const std::string_view arguments,
                                        const Color side_to_move) {
    std::istringstream stream{std::string(arguments)};
    const std::vector<std::string> tokens = remaining_tokens(stream);
    std::ostringstream translated;
    std::optional<std::int64_t> ucci_time;
    std::optional<std::int64_t> ucci_opptime;
    std::optional<std::int64_t> ucci_inc;
    std::optional<std::int64_t> ucci_oppinc;

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (index + 1 < tokens.size()) {
            std::int64_t val = 0;
            if (token == "time" && parse_int64_token(tokens[index + 1], val) && val >= 0) {
                ucci_time = val;
                ++index;
                continue;
            }
            if (token == "opptime" && parse_int64_token(tokens[index + 1], val) && val >= 0) {
                ucci_opptime = val;
                ++index;
                continue;
            }
            if (token == "increment" && parse_int64_token(tokens[index + 1], val) && val >= 0) {
                ucci_inc = val;
                ++index;
                continue;
            }
            if (token == "oppincrement" && parse_int64_token(tokens[index + 1], val) && val >= 0) {
                ucci_oppinc = val;
                ++index;
                continue;
            }
        }
        if (translated.tellp() > 0) {
            translated << ' ';
        }
        translated << token;
    }

    if (ucci_time.has_value()) {
        if (translated.tellp() > 0) {
            translated << ' ';
        }
        if (side_to_move == Color::white) {
            translated << "wtime " << *ucci_time;
            if (ucci_opptime.has_value()) translated << " btime " << *ucci_opptime;
            if (ucci_inc.has_value()) translated << " winc " << *ucci_inc;
            if (ucci_oppinc.has_value()) translated << " binc " << *ucci_oppinc;
        } else {
            translated << "btime " << *ucci_time;
            if (ucci_opptime.has_value()) translated << " wtime " << *ucci_opptime;
            if (ucci_inc.has_value()) translated << " binc " << *ucci_inc;
            if (ucci_oppinc.has_value()) translated << " winc " << *ucci_oppinc;
        }
    }
    return translated.str();
}

std::string translate_usi_go_arguments(const std::string_view arguments) {
    std::istringstream stream{std::string(arguments)};
    const std::vector<std::string> tokens = remaining_tokens(stream);
    std::ostringstream translated;
    bool has_explicit = false;
    std::optional<std::int64_t> usi_byoyomi;

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        if (token == "byoyomi" && index + 1 < tokens.size()) {
            std::int64_t val = 0;
            if (parse_int64_token(tokens[index + 1], val) && val >= 0) {
                usi_byoyomi = val;
                ++index;
                continue;
            }
        }
        if (token == "depth" || token == "nodes" || token == "movetime" ||
            token == "wtime" || token == "btime" || token == "infinite" ||
            token == "ponder" || token == "mate") {
            has_explicit = true;
        }
        if (translated.tellp() > 0) {
            translated << ' ';
        }
        translated << token;
    }

    if (usi_byoyomi.has_value() && !has_explicit) {
        if (translated.tellp() > 0) {
            translated << ' ';
        }
        translated << "movetime " << std::max<std::int64_t>(1, *usi_byoyomi);
    }
    return translated.str();
}

std::optional<std::pair<std::string, std::string>> parse_xboard_option_assignment(
    const std::string_view text) {
    const std::size_t eq = text.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view name = trim_ascii(text.substr(0, eq));
    if (name.empty()) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{std::string(name),
                                               std::string(text.substr(eq + 1))};
}

std::optional<std::chrono::milliseconds> parse_xboard_centiseconds_ms(
    const std::string_view token) noexcept {
    std::int64_t cs = 0;
    if (!parse_int64_token(token, cs)) {
        return std::nullopt;
    }
    constexpr std::int64_t kMaximumCentiseconds =
        std::numeric_limits<std::int64_t>::max() / 10;
    const std::int64_t clamped_cs = std::clamp<std::int64_t>(cs, 0, kMaximumCentiseconds);
    return std::chrono::milliseconds{std::max<std::int64_t>(1, clamped_cs * 10)};
}

bool is_bare_coordinate_move_token(const std::string_view token) noexcept {
    if (token.size() != 4 && token.size() != 5) {
        return false;
    }
    if (token[0] < 'a' || token[0] > 'h' || token[1] < '1' || token[1] > '8' ||
        token[2] < 'a' || token[2] > 'h' || token[3] < '1' || token[3] > '8') {
        return false;
    }
    if (token.size() == 5) {
        const char promo = ascii_lower(token[4]);
        return promo == 'n' || promo == 'b' || promo == 'r' || promo == 'q';
    }
    return true;
}

std::optional<ParsedCliPosition> parse_cli_position_with_history(
    const std::string_view spec, std::string& error) {
    error.clear();
    std::istringstream stream{std::string(spec)};
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) {
        tokens.push_back(std::move(token));
    }
    if (tokens.empty()) {
        return ParsedCliPosition{GameState::startpos(), {}, GameState::startpos()};
    }

    GameState state = GameState::startpos();
    std::size_t move_start = tokens.size();
    if (tokens[0] == "startpos") {
        if (tokens.size() > 1) {
            if (tokens[1] != "moves") {
                error = "invalid FEN '" + std::string(spec) + "'";
                return std::nullopt;
            }
            move_start = 2;
        }
    } else if (tokens[0] == "moves") {
        move_start = 1;
    } else {
        const std::size_t fen_start = (tokens[0] == "fen") ? 1 : 0;
        std::size_t moves_index = fen_start;
        while (moves_index < tokens.size() && tokens[moves_index] != "moves") {
            ++moves_index;
        }
        std::string fen_text = join_tokens(tokens, fen_start, moves_index);
        if (fen_text.empty()) {
            error = "invalid FEN '" + std::string(spec) + "'";
            return std::nullopt;
        }
        const std::string original_fen = fen_text;
        if (moves_index - fen_start == 4) {
            fen_text += " 0 1";
        }
        const auto parsed = GameState::from_fen(fen_text);
        if (!parsed.has_value()) {
            error = "invalid FEN '" + original_fen + "'";
            return std::nullopt;
        }
        state = *parsed;
        if (moves_index < tokens.size()) {
            move_start = moves_index + 1;
        }
    }

    const GameState origin = state;
    std::vector<Move> moves;
    moves.reserve(tokens.size() > move_start ? tokens.size() - move_start : 0);
    for (std::size_t index = move_start; index < tokens.size(); ++index) {
        const std::optional<Move> move = parse_move_uci_or_san(state, tokens[index]);
        if (!move.has_value() || !state.make_move(*move)) {
            error = "illegal move '" + tokens[index] + "'";
            return std::nullopt;
        }
        moves.push_back(*move);
    }
    return ParsedCliPosition{origin, std::move(moves), std::move(state)};
}

std::optional<GameState> parse_cli_position_spec(const std::string_view spec, std::string& error) {
    auto parsed = parse_cli_position_with_history(spec, error);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return std::move(parsed->state);
}

std::optional<ParsedEpdPrefix> parse_epd_prefix(std::string_view line) {
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    if (line.starts_with(kUtf8Bom)) {
        line.remove_prefix(kUtf8Bom.size());
    }
    std::size_t cursor = 0;
    std::array<std::string_view, 4> fields{};
    for (std::size_t field_index = 0; field_index < fields.size(); ++field_index) {
        while (cursor < line.size() && is_ascii_space(line[cursor])) {
            ++cursor;
        }
        const std::size_t start = cursor;
        while (cursor < line.size() && !is_ascii_space(line[cursor])) {
            ++cursor;
        }
        if (start == cursor) {
            return std::nullopt;
        }
        fields[field_index] = line.substr(start, cursor - start);
    }
    const std::string_view operations = trim_ascii(line.substr(cursor));
    if (!operations.empty() && operations.find(';') == std::string_view::npos) {
        return std::nullopt;
    }
    std::string four_field_fen;
    four_field_fen.reserve(fields[0].size() + fields[1].size() + fields[2].size() +
                           fields[3].size() + 4);
    four_field_fen.append(fields[0]).push_back(' ');
    four_field_fen.append(fields[1]).push_back(' ');
    four_field_fen.append(fields[2]).push_back(' ');
    four_field_fen.append(fields[3]);
    const auto state = GameState::from_fen(four_field_fen + " 0 1");
    if (!state.has_value()) {
        return std::nullopt;
    }
    return ParsedEpdPrefix{*state, std::move(four_field_fen), operations};
}

std::string format_position_debug_json(const std::uint64_t generation,
                                       const std::string_view command_text,
                                       const PositionConsistencySnapshot& snapshot,
                                       const Square en_passant_square,
                                       const std::uint64_t legal_digest) {
    const std::string digest_hex = debug_hex(legal_digest);
    return "\"message\":" + debug_quoted(
               "position_record command=" + std::string(command_text) +
               " legal_digest=" + digest_hex) +
           ",\"command\":" + debug_quoted(command_text) +
           ",\"generation\":" + std::to_string(generation) +
           ",\"root_fen\":" + debug_quoted(snapshot.native_fen) +
           ",\"position_key\":" + debug_quoted(debug_hex(snapshot.native_position_key)) +
           ",\"side_to_move\":" + debug_quoted(
               snapshot.native_side_to_move == Color::white ? "white" : "black") +
           ",\"castling_rights\":" + debug_quoted(
               debug_castling(snapshot.native_castling_rights)) +
           ",\"en_passant\":" + debug_quoted(debug_square(en_passant_square)) +
           ",\"halfmove_clock\":" + std::to_string(snapshot.native_halfmove_clock) +
           ",\"fullmove_number\":" + std::to_string(snapshot.native_fullmove_number) +
           ",\"legal_move_count\":" + std::to_string(snapshot.native_legal_moves.size()) +
           ",\"native_legal_count\":" + std::to_string(snapshot.native_legal_moves.size()) +
           ",\"shadow_legal_count\":" + std::to_string(snapshot.shadow_legal_moves.size()) +
           ",\"legal_move_digest\":" + debug_quoted(digest_hex) +
           ",\"native_shadow_consistent\":" +
               (snapshot.consistent() ? "true" : "false");
}

std::string format_hash_resize_debug_json(const HashResizeResult& result) {
    return "\"requested_mb\":" + std::to_string(result.requested_mb) +
           ",\"effective_mb\":" + std::to_string(result.effective_mb) +
           ",\"allocated_bytes\":" + std::to_string(result.allocated_bytes) +
           ",\"segment_count\":" + std::to_string(result.segment_count) +
           ",\"total_physical_bytes\":" + std::to_string(result.total_physical_bytes) +
           ",\"available_physical_bytes\":" +
               std::to_string(result.available_physical_bytes) +
           ",\"available_commit_bytes\":" + std::to_string(result.available_commit_bytes) +
           ",\"status\":" + debug_quoted(hash_status_name(result.status)) +
           ",\"reason\":" + debug_quoted(hash_reason_name(result.reason));
}

std::string format_go_debug_json(const std::uint64_t generation, const GameState& root,
                                 const SearchLimits& limits, const std::size_t threads,
                                 const bool from_book) {
    const std::string fen = root.fen();
    return "\"message\":" + debug_quoted(
               "go_record generation=" + std::to_string(generation) +
               " root_fen=" + fen) +
           ",\"generation\":" + std::to_string(generation) +
           ",\"root_fen\":" + debug_quoted(fen) +
           ",\"root_key\":" + debug_quoted(debug_hex(root.position_key())) +
           ",\"limits\":" + debug_quoted(debug_limits(limits)) +
           ",\"threads\":" + std::to_string(threads) +
           ",\"source\":" + debug_quoted(from_book ? "book" : "search");
}

std::string format_search_completion_debug_json(
    const std::uint64_t generation, const GameState& search_root,
    const CompletionCandidate& candidate, const CompletionValidation& validation,
    const SearchResult& result, const std::string_view source,
    const std::string_view last_command) {
    const std::string root_fen = search_root.fen();
    const std::uint64_t root_key = search_root.position_key();
    return "\"message\":" + debug_quoted(
               "completion_record generation=" + std::to_string(generation)) +
           ",\"generation\":" + std::to_string(generation) +
           ",\"result_generation\":" + std::to_string(result.identity.generation) +
           ",\"root_fen\":" + debug_quoted(root_fen) +
           ",\"root_key\":" + debug_quoted(debug_hex(root_key)) +
           ",\"result_root_key\":" + debug_quoted(debug_hex(result.identity.root_key)) +
           ",\"root_key_match\":" +
               (result.identity.root_key == root_key ? "true" : "false") +
           ",\"root_fen_match\":" + (result.identity.root_fen == root_fen ? "true" : "false") +
           ",\"identity_match\":" + (validation.identity_match ? "true" : "false") +
           ",\"candidate\":" + debug_quoted(
               candidate.best_move.has_value() ? candidate.best_move->uci() : "0000") +
           ",\"native_legal\":" + (validation.native_legal ? "true" : "false") +
           ",\"shadow_legal\":" + (validation.shadow_legal ? "true" : "false") +
           ",\"searchmoves_legal\":" + (validation.searchmoves_legal ? "true" : "false") +
           ",\"pv_legal\":" + (validation.pv_legal ? "true" : "false") +
           ",\"ponder_legal\":" + (validation.ponder_legal ? "true" : "false") +
           ",\"fallback_used\":" + (validation.fallback_used ? "true" : "false") +
           ",\"validated\":" + debug_quoted(
               validation.best_move.has_value() ? validation.best_move->uci() : "0000") +
           ",\"source\":" + debug_quoted(source) +
           ",\"disposition\":" + debug_quoted(disposition_name(validation.disposition)) +
           ",\"reason\":" + debug_quoted(validation.reason) +
           ",\"completed\":" + (result.completed ? "true" : "false") +
           ",\"cancelled\":" + (result.cancelled ? "true" : "false") +
           ",\"failed\":" + (result.failed ? "true" : "false") +
           ",\"timing_reserve_ms\":" + std::to_string(result.timing.reserve.count()) +
           ",\"timing_usable_ms\":" + std::to_string(result.timing.usable.count()) +
           ",\"timing_soft_ms\":" + std::to_string(result.timing.soft_budget.count()) +
           ",\"timing_hard_ms\":" + std::to_string(result.timing.hard_budget.count()) +
           ",\"timing_horizon\":" + std::to_string(result.timing.horizon) +
           ",\"timing_initial_hardness\":" +
               std::to_string(result.timing.initial_hardness) +
           ",\"timing_observed_hardness\":" +
               std::to_string(result.timing.observed_hardness) +
           ",\"timing_extended\":" +
               (result.timing.extended_for_hard_position ? "true" : "false") +
           ",\"timing_hard_deadline\":" +
               (result.timing.hard_deadline_reached ? "true" : "false") +
           ",\"last_command\":" + debug_quoted(last_command);
}

std::string format_book_completion_debug_json(
    const std::uint64_t generation, const GameState& root,
    const CompletionCandidate& candidate, const CompletionValidation& validation,
    const Move book_move, const std::string_view last_command) {
    std::array<char, 5> book_buf{};
    std::array<char, 5> validated_buf{};
    return "\"message\":" + debug_quoted(
               "book_completion_record generation=" + std::to_string(generation)) +
           ",\"generation\":" + std::to_string(generation) +
           ",\"result_generation\":" + std::to_string(candidate.identity.generation) +
           ",\"root_fen\":" + debug_quoted(root.fen()) +
           ",\"root_key\":" + debug_quoted(debug_hex(root.position_key())) +
           ",\"result_root_key\":" + debug_quoted(debug_hex(candidate.identity.root_key)) +
           ",\"root_key_match\":true" +
           ",\"root_fen_match\":true" +
           ",\"identity_match\":" + (validation.identity_match ? "true" : "false") +
           ",\"candidate\":" + debug_quoted(book_move.format_uci(book_buf)) +
           ",\"native_legal\":" + (validation.native_legal ? "true" : "false") +
           ",\"shadow_legal\":" + (validation.shadow_legal ? "true" : "false") +
           ",\"searchmoves_legal\":" + (validation.searchmoves_legal ? "true" : "false") +
           ",\"pv_legal\":" + (validation.pv_legal ? "true" : "false") +
           ",\"ponder_legal\":" + (validation.ponder_legal ? "true" : "false") +
           ",\"fallback_used\":" + (validation.fallback_used ? "true" : "false") +
           ",\"validated\":" + debug_quoted(
               validation.best_move.has_value()
                   ? validation.best_move->format_uci(validated_buf)
                   : std::string_view("0000")) +
           ",\"source\":\"book\"" +
           ",\"disposition\":" + debug_quoted(disposition_name(validation.disposition)) +
           ",\"reason\":" + debug_quoted(validation.reason) +
           ",\"last_command\":" + debug_quoted(last_command);
}

std::string format_search_move_debug_json(
    const std::uint64_t generation, const GameState& search_root,
    const std::optional<Move>& best_move, const std::string_view source) {
    std::string after_fen = search_root.fen();
    if (best_move.has_value()) {
        GameState after_move = search_root;
        if (after_move.make_move(*best_move)) {
            after_fen = after_move.fen();
        }
    }
    return "\"generation\":" + std::to_string(generation) +
           ",\"root_fen\":" + debug_quoted(search_root.fen()) +
           ",\"root_key\":" + debug_quoted(debug_hex(search_root.position_key())) +
           ",\"move\":" + debug_quoted(best_move.has_value() ? best_move->uci() : "0000") +
           ",\"after_fen\":" + debug_quoted(after_fen) +
           ",\"source\":" + debug_quoted(source);
}

std::string format_book_move_debug_json(
    const std::uint64_t generation, const GameState& root,
    const std::optional<Move>& best_move, const std::size_t book_depth) {
    return "\"generation\":" + std::to_string(generation) +
           ",\"root_fen\":" + debug_quoted(root.fen()) +
           ",\"root_key\":" + debug_quoted(debug_hex(root.position_key())) +
           ",\"move\":" + debug_quoted(best_move.has_value() ? best_move->uci() : "0000") +
           ",\"source\":\"book\"" +
           ",\"book_depth\":" + std::to_string(book_depth);
}

std::vector<std::string> format_perft_divide_lines(
    const std::vector<std::pair<Move, std::uint64_t>>& counts,
    const std::uint64_t total_nodes, const std::int64_t elapsed_ms) {
    const auto safe_elapsed_ms = std::max<std::int64_t>(1, elapsed_ms);
    const std::uint64_t nps =
        (total_nodes * 1000ULL) / static_cast<std::uint64_t>(safe_elapsed_ms);
    std::vector<std::string> lines;
    lines.reserve(counts.size() + 2);
    for (const auto& [move, nodes] : counts) {
        lines.push_back(move.uci() + ": " + std::to_string(nodes));
    }
    lines.push_back("Nodes searched: " + std::to_string(total_nodes));
    lines.push_back("Time (ms): " + std::to_string(safe_elapsed_ms) +
                    " | Nodes/second: " + std::to_string(nps));
    return lines;
}

std::string format_bench_summary(const std::uint64_t total_nodes,
                                 const std::int64_t elapsed_ms) {
    const auto safe_elapsed_ms = std::max<std::int64_t>(1, elapsed_ms);
    const std::uint64_t nps =
        (total_nodes * 1000ULL) / static_cast<std::uint64_t>(safe_elapsed_ms);
    return "\n===========================\nTotal time (ms) : " +
           std::to_string(safe_elapsed_ms) + "\nNodes searched  : " +
           std::to_string(total_nodes) + "\nNodes/second    : " +
           std::to_string(nps) + "\n";
}

std::string format_speedtest_summary(const std::size_t threads, const std::size_t hash_mb,
                                     const std::uint64_t total_nodes,
                                     const std::int64_t elapsed_ms) {
    const auto safe_elapsed_ms = std::max<std::int64_t>(1, elapsed_ms);
    const std::uint64_t nps =
        (total_nodes * 1000ULL) / static_cast<std::uint64_t>(safe_elapsed_ms);
    return "\n===========================\nThreads         : " + std::to_string(threads) +
           "\nHash (MB)       : " + std::to_string(hash_mb) +
           "\nTotal time (ms) : " + std::to_string(safe_elapsed_ms) +
           "\nNodes searched  : " + std::to_string(total_nodes) +
           "\nNodes/second    : " + std::to_string(nps) + "\n";
}

std::string format_see_result(const std::string_view move_uci, const int gain_cp) {
    std::ostringstream out;
    out << "SEE " << move_uci << " : " << std::showpos << gain_cp << " cp\n";
    return out.str();
}

std::optional<std::string> move_to_san(const GameState& state, const Move move) {
    if (move.is_no_move() || !state.is_legal(move)) {
        return std::nullopt;
    }
    const Piece piece = state.piece_at(move.from());
    if (piece.empty()) {
        return std::nullopt;
    }

    auto piece_letter = [](const PieceType type) noexcept -> char {
        switch (type) {
        case PieceType::knight: return 'N';
        case PieceType::bishop: return 'B';
        case PieceType::rook:   return 'R';
        case PieceType::queen:  return 'Q';
        case PieceType::king:   return 'K';
        default:                return '\0';
        }
    };
    auto promotion_letter = [](const Promotion promotion) noexcept -> char {
        switch (promotion) {
        case Promotion::knight: return 'N';
        case Promotion::bishop: return 'B';
        case Promotion::rook:   return 'R';
        case Promotion::queen:  return 'Q';
        case Promotion::none:   return '\0';
        }
        return '\0';
    };

    std::string san;
    san.reserve(8);
    const int file_delta =
        static_cast<int>(move.to().index() % 8) - static_cast<int>(move.from().index() % 8);
    if (piece.type == PieceType::king && std::abs(file_delta) == 2) {
        san = file_delta > 0 ? "O-O" : "O-O-O";
    } else {
        const bool capture = state.is_capture(move);
        if (piece.type == PieceType::pawn) {
            if (capture) {
                san.push_back(move.from().file());
                san.push_back('x');
            }
        } else {
            san.push_back(piece_letter(piece.type));
            if (piece.type != PieceType::king) {
                std::array<Move, kMaximumLegalMoves> legal{};
                const std::size_t legal_count =
                    std::min(state.legal_moves_into(legal), legal.size());
                bool any_other = false;
                bool same_file = false;
                bool same_rank = false;
                for (std::size_t index = 0; index < legal_count; ++index) {
                    const Move other = legal[index];
                    if (other.to() != move.to() || other.from() == move.from()) {
                        continue;
                    }
                    if (state.piece_at(other.from()).type == piece.type) {
                        any_other = true;
                        if (other.from().file() == move.from().file()) {
                            same_file = true;
                        }
                        if (other.from().rank() == move.from().rank()) {
                            same_rank = true;
                        }
                    }
                }
                if (any_other) {
                    if (!same_file || same_rank) {
                        san.push_back(move.from().file());
                    }
                    if (same_file) {
                        san.push_back(move.from().rank());
                    }
                }
            }
            if (capture) {
                san.push_back('x');
            }
        }
        san.push_back(move.to().file());
        san.push_back(move.to().rank());
        if (const char promo = promotion_letter(move.promotion()); promo != '\0') {
            san.push_back('=');
            san.push_back(promo);
        }
    }

    GameState after = state;
    after.detach_mirror();
    if (after.make_move(move) && after.in_check()) {
        san.push_back(after.has_legal_move() ? '+' : '#');
    }
    return san;
}

std::optional<Move> parse_san_move(const GameState& state, std::string_view token) {
    while (!token.empty() && (token.back() == '+' || token.back() == '#' ||
                              token.back() == '!' || token.back() == '?')) {
        token.remove_suffix(1);
    }
    if (token.empty()) {
        return std::nullopt;
    }

    std::string normalized;
    if (token == "0-0" || equals_ignore_case(token, "o-o")) {
        normalized = "O-O";
    } else if (token == "0-0-0" || equals_ignore_case(token, "o-o-o")) {
        normalized = "O-O-O";
    } else {
        normalized = std::string(token);
        if (normalized.size() >= 3) {
            const char promo = normalized.back();
            const char dest_rank = normalized[normalized.size() - 2];
            if ((promo == 'Q' || promo == 'R' || promo == 'B' || promo == 'N') &&
                (dest_rank == '1' || dest_rank == '8')) {
                normalized.insert(normalized.end() - 1, '=');
            }
        }
    }

    std::array<Move, kMaximumLegalMoves> legal{};
    const std::size_t legal_count =
        std::min(state.legal_moves_into(legal), legal.size());
    for (std::size_t index = 0; index < legal_count; ++index) {
        const Move move = legal[index];
        const std::optional<std::string> san = move_to_san(state, move);
        if (!san.has_value()) {
            continue;
        }
        std::string_view candidate(*san);
        while (!candidate.empty() && (candidate.back() == '+' || candidate.back() == '#')) {
            candidate.remove_suffix(1);
        }
        if (candidate == normalized) {
            return move;
        }
    }
    return std::nullopt;
}

std::optional<Move> parse_move_uci_or_san(const GameState& state,
                                          const std::string_view token) {
    if (const std::optional<Move> uci = Move::parse_uci(token);
        uci.has_value() && state.is_legal(*uci)) {
        return uci;
    }
    return parse_san_move(state, token);
}

namespace {

std::string format_san_movetext(GameState& state, const std::vector<Move>& moves) {
    state.detach_mirror();
    std::ostringstream out;
    bool first = true;
    for (std::size_t index = 0; index < moves.size(); ++index) {
        const Move& move = moves[index];
        const std::string token = move_to_san(state, move).value_or(move.uci());
        if (!first) {
            out << ' ';
        }
        first = false;
        if (state.side_to_move() == Color::white) {
            out << state.fullmove_number() << ". " << token;
        } else if (index == 0) {
            out << state.fullmove_number() << "... " << token;
        } else {
            out << token;
        }
        if (!state.make_move(move)) {
            break;
        }
    }
    return out.str();
}

} // namespace

std::string format_move_history(const GameState& origin, const std::vector<Move>& moves) {
    const std::string origin_fen = origin.fen();
    const bool is_startpos = (origin_fen == kStartposFen);
    std::ostringstream out;
    out << "Origin  : " << (is_startpos ? "startpos" : origin_fen) << '\n'
        << "Plies   : " << moves.size() << '\n'
        << "Moves   :";
    if (moves.empty()) {
        out << " (none)\nSAN     : (none)";
    } else {
        std::array<char, 5> move_buffer{};
        for (const Move& move : moves) {
            out << ' ' << move.format_uci(move_buffer);
        }
        GameState final_state = origin;
        out << "\nSAN     : " << format_san_movetext(final_state, moves);
    }
    out << "\nCommand : position " << (is_startpos ? "startpos" : ("fen " + origin_fen));
    if (!moves.empty()) {
        std::array<char, 5> move_buffer{};
        out << " moves";
        for (const Move& move : moves) {
            out << ' ' << move.format_uci(move_buffer);
        }
    }
    out << '\n';
    return out.str();
}

std::string format_pgn(const GameState& origin, const std::vector<Move>& moves) {
    GameState final_state = origin;
    const std::string movetext = format_san_movetext(final_state, moves);
    const char* result = "*";
    if (!final_state.has_legal_move()) {
        result = final_state.in_check()
            ? (final_state.side_to_move() == Color::white ? "0-1" : "1-0")
            : "1/2-1/2";
    } else if (final_state.is_draw_by_rule()) {
        result = "1/2-1/2";
    }

    const std::string origin_fen = origin.fen();
    const bool is_startpos = (origin_fen == kStartposFen);
    std::ostringstream out;
    out << "[Event \"Koi Session\"]\n"
        << "[Site \"?\"]\n"
        << "[Date \"????.??.??\"]\n"
        << "[Round \"?\"]\n"
        << "[White \"?\"]\n"
        << "[Black \"?\"]\n"
        << "[Result \"" << result << "\"]\n";
    if (!is_startpos) {
        out << "[SetUp \"1\"]\n"
            << "[FEN \"" << origin_fen << "\"]\n";
    }
    out << '\n' << (movetext.empty() ? std::string(result) : (movetext + ' ' + result)) << '\n';
    return out.str();
}

std::string format_epd(const GameState& position) {
    const std::string fen = position.fen();
    std::array<std::string_view, 6> fields{};
    std::size_t count = 0;
    std::size_t cursor = 0;
    while (cursor < fen.size() && count < fields.size()) {
        while (cursor < fen.size() && is_ascii_space(fen[cursor])) {
            ++cursor;
        }
        const std::size_t start = cursor;
        while (cursor < fen.size() && !is_ascii_space(fen[cursor])) {
            ++cursor;
        }
        if (start < cursor) {
            fields[count++] = std::string_view(fen).substr(start, cursor - start);
        }
    }
    if (count < fields.size()) {
        return fen + '\n';
    }
    std::string epd;
    epd.reserve(fen.size() + 16);
    epd.append(fields[0]).push_back(' ');
    epd.append(fields[1]).push_back(' ');
    epd.append(fields[2]).push_back(' ');
    epd.append(fields[3]);
    epd += " hmvc ";
    epd.append(fields[4]);
    epd += "; fmvn ";
    epd.append(fields[5]);
    epd += ";\n";
    return epd;
}

PerftSuiteReport run_perft_suite() {
    PerftSuiteReport report;
    report.total = kPerftSuite.size();
    report.lines.reserve(kPerftSuite.size() + 1U);
    const auto started = std::chrono::steady_clock::now();
    for (const PerftSuiteCase& entry : kPerftSuite) {
        const auto parsed = GameState::from_fen(entry.fen);
        if (!parsed.has_value()) {
            report.lines.push_back(
                "FAIL " + std::string(entry.name) + " depth " + std::to_string(entry.depth) +
                " expected " + std::to_string(entry.expected_nodes) + " got 0");
            continue;
        }
        GameState state = *parsed;
        const std::uint64_t actual = koi::perft(state, entry.depth);
        report.total_nodes += actual;
        if (actual == entry.expected_nodes) {
            ++report.passed;
            report.lines.push_back(
                "PASS " + std::string(entry.name) + " depth " + std::to_string(entry.depth) +
                " nodes " + std::to_string(actual));
        } else {
            report.lines.push_back(
                "FAIL " + std::string(entry.name) + " depth " + std::to_string(entry.depth) +
                " expected " + std::to_string(entry.expected_nodes) + " got " +
                std::to_string(actual));
        }
    }
    report.elapsed_ms = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
               .count());
    const std::uint64_t nps =
        (report.total_nodes * 1000ULL) / static_cast<std::uint64_t>(report.elapsed_ms);
    report.lines.push_back(
        "suite " + std::to_string(report.passed) + '/' + std::to_string(report.total) +
        " passed | nodes " + std::to_string(report.total_nodes) +
        " | time_ms " + std::to_string(report.elapsed_ms) +
        " | nps " + std::to_string(nps));
    return report;
}

} // namespace koi::detail
