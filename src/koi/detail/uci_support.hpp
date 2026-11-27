#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "koi/completion_gate.hpp"
#include "koi/game_state.hpp"
#include "koi/hash_types.hpp"
#include "koi/move.hpp"
#include "koi/move_annotation.hpp"
#include "koi/search_types.hpp"

namespace koi {

struct BookChoice;
class Evaluator;
class SyzygyTablebase;

} // namespace koi

namespace koi::detail {

enum class BoundedLineRead : std::uint8_t {
    eof,
    line,
};

struct ParsedOption {
    std::string name;
    std::string value;
};

struct Wdl {
    int win = 0;
    int draw = 0;
    int loss = 0;
};

inline constexpr std::array<std::string_view, 16> kBenchFens{{
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
    "r1bqkb1r/1p2pppp/p1np1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R w KQkq - 0 6",
    "r1bq1rk1/2p1bppp/p1np1n2/1p2p3/4P3/1BP2N2/PP1P1PPP/RNBQR1K1 w - - 0 9",
    "r2q1rk1/pp2bppp/2n1bn2/3pp3/8/1BP2NP1/PP1P1PBP/RNBQ1RK1 b - - 0 8",
    "r2qkb1r/pp1n1ppp/2p1pn2/3p4/2PP4/2N2NP1/PP2PPBP/R1BQK2R b KQkq - 0 6",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "2r3k1/1q1nbppp/r3p3/3pP3/pPpP4/P1Q2N2/2RN1PPP/2R4K b - b3 0 23",
    "r1b1k2r/pp2qppp/2n2n2/2b5/3pP3/2N2N2/PP2BPPP/R1BQ1RK1 w kq - 0 10",
    "r2q1rk1/pb1nbppp/1p2pn2/2pp4/2PP4/1PN1PN2/PB2BPPP/R2Q1RK1 w - - 0 10",
    "3r2k1/p4ppp/1q2p3/2n5/4P3/1P3P2/P2Q2PP/3R2K1 w - - 0 24",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "8/5pk1/7p/3p4/3P4/5PP1/5K2/8 w - - 0 40",
    "4k3/2p2pp1/1p5p/8/3B4/1P3P2/2P2KPP/4b3 w - - 0 30",
    "8/1P3k2/8/8/8/5K2/6p1/8 w - - 0 55",
}};

// Reads one protocol line while capping the retained size at `limit` bytes.
// The rest of an oversized line is consumed (so the next command still starts
// at a line boundary) and `truncated` reports that it must be rejected.
[[nodiscard]] BoundedLineRead read_bounded_protocol_line(
    std::istream& input, std::string& line, std::size_t limit, bool& truncated);

[[nodiscard]] const char* disposition_name(CompletionDisposition disposition) noexcept;
[[nodiscard]] const char* hash_status_name(HashResizeStatus status) noexcept;
[[nodiscard]] const char* hash_reason_name(HashResizeReason reason) noexcept;

[[nodiscard]] std::vector<std::string> remaining_tokens(std::istream& command);
[[nodiscard]] std::string join_tokens(
    const std::vector<std::string>& tokens, std::size_t first, std::size_t last);
[[nodiscard]] bool equals_ignore_case(std::string_view left, std::string_view right) noexcept;

[[nodiscard]] std::optional<ParsedOption> parse_setoption(
    const std::vector<std::string>& tokens);
// UCCI allows `setoption <option> <value>` without `name`/`value` keywords in
// addition to standard `setoption name <option> value <value>`.
[[nodiscard]] std::optional<ParsedOption> parse_setoption_for_protocol(
    const std::vector<std::string>& tokens, bool allow_ucci_shorthand);

[[nodiscard]] bool parse_int64_token(std::string_view value, std::int64_t& parsed) noexcept;
[[nodiscard]] bool parse_uint64(std::string_view value, std::uint64_t& parsed) noexcept;
[[nodiscard]] bool parse_boolean(std::string_view value, bool& parsed);
[[nodiscard]] bool parse_random_seed(std::string_view value, std::uint32_t& seed);
[[nodiscard]] bool parse_milliseconds(std::string_view value, std::chrono::milliseconds& duration);
[[nodiscard]] bool parse_xboard_seconds(std::string_view value, std::chrono::milliseconds& duration);
[[nodiscard]] bool parse_xboard_level_time(
    std::string_view value, std::chrono::milliseconds& duration);

[[nodiscard]] int cecp_mate_score(int mate_in_moves) noexcept;
[[nodiscard]] std::string xboard_terminal_result_line(const GameState& state);
[[nodiscard]] std::uint32_t root_ply(const GameState& state);
[[nodiscard]] std::string format_eval_tokens(const MoveScoreEval& eval);
[[nodiscard]] Wdl score_to_wdl(const SearchInfo& info) noexcept;

[[nodiscard]] std::string debug_quoted(std::string_view value);
[[nodiscard]] std::string debug_square(Square square);
[[nodiscard]] std::string debug_castling(std::uint8_t rights);
[[nodiscard]] std::string debug_hex(std::uint64_t value);
[[nodiscard]] std::uint64_t legal_move_digest(const std::vector<Move>& legal_moves);
[[nodiscard]] std::string debug_limits(const SearchLimits& limits);

[[nodiscard]] std::vector<Move> legal_pv_prefix(
    const GameState& root, const std::vector<Move>& pv);
[[nodiscard]] bool parse_annotate_limits(
    const std::vector<std::string>& tokens, std::size_t first_index,
    SearchLimits& limits, std::string& error);

[[nodiscard]] SearchLimits parse_go_limits(
    std::string_view arguments,
    std::chrono::milliseconds bare_go_fallback = std::chrono::milliseconds{250});
[[nodiscard]] std::uint32_t derive_network_overhead_ms(
    std::int64_t previous_remaining_ms, std::int64_t increment_ms,
    std::int64_t elapsed_ms, std::int64_t current_remaining_ms,
    std::uint32_t previous_estimate_ms) noexcept;

[[nodiscard]] std::string format_ascii_board(const GameState& position);
[[nodiscard]] std::string format_key_info(const GameState& position);
[[nodiscard]] std::string format_legal_moves(const GameState& position);
[[nodiscard]] std::string format_book_entries(
    std::string_view book_name, const std::vector<BookChoice>& entries);
[[nodiscard]] std::string format_syzygy_probe(
    const SyzygyTablebase* syzygy, const GameState& position);
[[nodiscard]] std::string format_eval_summary(
    const GameState& position, const Evaluator* evaluator, bool hce_active);
[[nodiscard]] std::string format_compiler_info(std::string_view version, std::string_view commit);
[[nodiscard]] std::string format_interactive_help();

} // namespace koi::detail
