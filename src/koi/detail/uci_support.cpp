#include "koi/detail/uci_support.hpp"

#include "koi/detail/uci_option_table.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
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

const char* disposition_name(const CompletionDisposition disposition) noexcept {
    switch (disposition) {
    case CompletionDisposition::emit: return "emit";
    case CompletionDisposition::suppress_stale: return "suppress_stale";
    case CompletionDisposition::fallback: return "fallback";
    case CompletionDisposition::quarantine: return "quarantine";
    }
    return "unknown";
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
        const auto left_character = static_cast<unsigned char>(left[index]);
        const auto right_character = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_character) != std::tolower(right_character)) {
            return false;
        }
    }
    return true;
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

bool parse_int64_token(const std::string_view value, std::int64_t& parsed) noexcept {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

bool parse_uint64(const std::string_view value, std::uint64_t& parsed) noexcept {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

bool parse_boolean(const std::string_view value, bool& parsed) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) {
        normalized.push_back(static_cast<char>(std::tolower(character)));
    }

    if (normalized == "true") {
        parsed = true;
        return true;
    }
    if (normalized == "false") {
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
    if (state.legal_moves().empty()) {
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

std::string format_eval_tokens(const MoveScoreEval& eval) {
    if (eval.mate.has_value()) {
        return "mate " + std::to_string(*eval.mate);
    }
    return "cp " + std::to_string(eval.score_cp);
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

std::string debug_hex(const std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::uint64_t legal_move_digest(const std::vector<Move>& legal_moves) {
    std::vector<std::string> coordinates;
    coordinates.reserve(legal_moves.size());
    for (const Move& move : legal_moves) {
        coordinates.push_back(move.uci());
    }
    std::sort(coordinates.begin(), coordinates.end());

    std::uint64_t digest = 1469598103934665603ULL;
    for (const std::string& coordinate : coordinates) {
        for (const unsigned char character : coordinate) {
            digest ^= character;
            digest *= 1099511628211ULL;
        }
        digest ^= static_cast<unsigned char>('\n');
        digest *= 1099511628211ULL;
    }
    return digest;
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
        stream << ",searchmove_list=";
        for (std::size_t index = 0; index < limits.search_moves.size(); ++index) {
            if (index != 0) stream << ',';
            stream << limits.search_moves[index].uci();
        }
    }
    return stream.str();
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
    buffer << "   a   b   c   d   e   f   g   h\n\n"
           << "Fen   : " << fen << '\n'
           << "Key   : " << std::hex << std::setfill('0') << std::setw(16) << key
           << std::dec << std::setfill(' ') << '\n'
           << "Side  : " << (white_to_move ? "white" : "black") << '\n'
           << "Check : " << (checked ? "yes" : "no") << '\n'
           << "Moves : " << position.legal_moves().size() << '\n';
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
        "  key                                 Print Zobrist position and pawn hash keys\n"
        "  moves                               List sorted legal UCI moves from current position\n"
        "  eval                                Show active evaluator, score, WDL, and phase\n"
        "  flip                                Flip side to move when legal\n"
        "  see <move>                          Show static exchange evaluation for a legal move\n"
        "  perft <depth>                       Run interruptible perft divide (depth 1..10)\n"
        "  bench [depth [hash [threads]]]      Run OpenBench-compatible benchmark\n"
        "  compiler                            Show compiler, C++ standard, and ISA\n"
        "  help                                Show this command summary\n"
        "  quit                                Exit the engine\n";
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

} // namespace koi::detail
