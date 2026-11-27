#include "koi/detail/uci_support.hpp"

#include "koi/detail/uci_option_table.hpp"
#include "koi/classical_evaluator.hpp"
#include "koi/evaluator.hpp"
#include "koi/nnue.hpp"
#include "koi/opening_book.hpp"
#include "koi/syzygy_tablebase.hpp"

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

std::string format_key_info(const GameState& position) {
    std::ostringstream buffer;
    buffer << "Position key : " << debug_hex(position.position_key()) << '\n'
           << "Pawn key     : " << debug_hex(position.pawn_key()) << '\n'
           << "Polyglot key : " << debug_hex(position.polyglot_key()) << '\n';
    return buffer.str();
}

std::string format_legal_moves(const GameState& position) {
    const std::vector<Move> raw_moves = position.legal_moves();
    std::vector<std::string> moves;
    moves.reserve(raw_moves.size());
    for (const Move& move : raw_moves) {
        moves.push_back(move.uci());
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
    for (const BookChoice& entry : entries) {
        const double pct = total_weight > 0
            ? (100.0 * static_cast<double>(entry.weight) / static_cast<double>(total_weight))
            : 0.0;
        buffer << "  " << entry.move.uci() << "  weight=" << entry.weight
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
            for (const Move& move : root->moves) {
                buffer << ' ' << move.uci();
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
    buffer << "Evaluator      : ";
    if (hce_active) {
        buffer << "HCE (classical tapered)";
    } else if (const auto* nnue = dynamic_cast<const NnueEvaluator*>(evaluator);
               nnue != nullptr && nnue->network() != nullptr) {
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
    if (!hce_active) {
        if (const auto* nnue = dynamic_cast<const NnueEvaluator*>(evaluator);
            nnue != nullptr && nnue->network() != nullptr) {
            const NnueNetwork& net = *nnue->network();
            const NnueManifest& m = net.manifest;
            buffer << "Network SHA256 : " << std::hex << std::nouppercase << std::setfill('0');
            for (std::size_t i = 0; i < 6; ++i) {
                buffer << std::setw(2) << static_cast<unsigned>(m.network_sha256[i]);
            }
            buffer << std::dec << std::setfill(' ') << '\n';
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
        "  key                                 Print Zobrist position, pawn, and Polyglot hash keys\n"
        "  moves                               List sorted legal UCI moves from current position\n"
        "  book                                Probe active Polyglot opening book entries\n"
        "  tb                                  Probe active Syzygy tablebases for root WDL/moves\n"
        "  eval                                Show active evaluator, score, WDL, and phase\n"
        "  flip                                Flip side to move when legal\n"
        "  see <move>                          Show static exchange evaluation for a legal move\n"
        "  perft <depth>                       Run interruptible perft divide (depth 1..10)\n"
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
    line << std::fixed;
    line << "annotate move " << result.played_move.uci()
         << " ply " << result.ply
         << " side " << (result.side_to_move == Color::white ? "white" : "black")
         << " best " << result.best_move.uci()
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
            line << ' ' << pv_move.uci();
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
    std::string_view name = text.substr(0, eq);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())) != 0) {
        name.remove_prefix(1);
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0) {
        name.remove_suffix(1);
    }
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
        const char promo =
            static_cast<char>(std::tolower(static_cast<unsigned char>(token[4])));
        return promo == 'n' || promo == 'b' || promo == 'r' || promo == 'q';
    }
    return true;
}

} // namespace koi::detail
