#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
struct NnueNetwork;
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

struct LiveOptionsSnapshot {
    std::string_view protocol_name = "uci";
    std::string evaluator_name;
    std::string eval_file;
    std::size_t hash_mb = 512;
    std::size_t threads = 1;
    std::size_t multi_pv = 1;
    bool ponder = false;
    bool analyse_mode = false;
    bool show_wdl = false;
    std::uint32_t contempt_cp = 0;
    std::uint32_t move_overhead_ms = 30;
    bool dynamic_overhead = false;
    std::uint32_t observed_gui_overhead_ms = 0;
    std::uint32_t slow_mover_percent = 100;
    std::uint32_t random_seed = 0;
    bool own_book = false;
    bool own_book_explicit = false;
    bool book_available = false;
    std::string book_file = "book.bin";
    std::uint8_t book_depth = 16;
    bool book_random = false;
    bool book_safety = true;
    std::uint8_t book_safety_depth = 2;
    std::string syzygy_path;
    bool syzygy_auto_detected = false;
    std::uint8_t syzygy_probe_depth = 1;
    std::uint8_t syzygy_probe_limit = 7;
    std::uint8_t syzygy_interior_depth = 0;
    bool syzygy_50_move_rule = true;
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

[[nodiscard]] const char* hash_status_name(HashResizeStatus status) noexcept;
[[nodiscard]] const char* hash_reason_name(HashResizeReason reason) noexcept;

[[nodiscard]] std::vector<std::string> remaining_tokens(std::istream& command);
[[nodiscard]] std::string join_tokens(
    const std::vector<std::string>& tokens, std::size_t first, std::size_t last);
[[nodiscard]] bool equals_ignore_case(std::string_view left, std::string_view right) noexcept;

// UCCI allows `setoption <option> <value>` without `name`/`value` keywords in
// addition to standard `setoption name <option> value <value>`.
[[nodiscard]] std::optional<ParsedOption> parse_setoption_for_protocol(
    const std::vector<std::string>& tokens, bool allow_ucci_shorthand);

[[nodiscard]] bool parse_uint64(std::string_view value, std::uint64_t& parsed) noexcept;
[[nodiscard]] bool parse_boolean(std::string_view value, bool& parsed);
[[nodiscard]] bool parse_random_seed(std::string_view value, std::uint32_t& seed);
[[nodiscard]] bool parse_xboard_seconds(std::string_view value, std::chrono::milliseconds& duration);
[[nodiscard]] bool parse_xboard_level_time(
    std::string_view value, std::chrono::milliseconds& duration);

[[nodiscard]] int cecp_mate_score(int mate_in_moves) noexcept;
[[nodiscard]] std::string xboard_terminal_result_line(const GameState& state);
[[nodiscard]] std::uint32_t root_ply(const GameState& state);
[[nodiscard]] Wdl score_to_wdl(const SearchInfo& info) noexcept;

[[nodiscard]] std::string debug_quoted(std::string_view value);
[[nodiscard]] std::uint64_t legal_move_digest(const std::vector<Move>& legal_moves);

[[nodiscard]] std::vector<Move> legal_pv_prefix(
    const GameState& root, const std::vector<Move>& pv);
[[nodiscard]] bool parse_annotate_limits(
    const std::vector<std::string>& tokens, std::size_t first_index,
    SearchLimits& limits, std::string& error);

[[nodiscard]] SearchLimits parse_go_limits(
    std::string_view arguments,
    std::chrono::milliseconds bare_go_fallback = std::chrono::milliseconds{250});
[[nodiscard]] std::string translate_ucci_go_arguments(
    std::string_view arguments, Color side_to_move);
[[nodiscard]] std::string translate_usi_go_arguments(std::string_view arguments);
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
[[nodiscard]] std::string format_annotate_move_line(const MoveAnnotationResult& result);
[[nodiscard]] std::string format_annotate_summary_line(
    const char* side_name, const PlayerAnnotationSummary& summary);
[[nodiscard]] std::optional<std::string> flip_position_fen(std::string_view fen);
[[nodiscard]] std::optional<GameState> parse_cli_position_spec(
    std::string_view spec, std::string& error);
[[nodiscard]] std::string format_compiler_info(std::string_view version, std::string_view commit);
[[nodiscard]] std::string format_nnue_info(
    std::string_view display_path, const NnueNetwork& network);
[[nodiscard]] std::string format_options_summary(const LiveOptionsSnapshot& snapshot);
[[nodiscard]] std::vector<std::string> format_perft_divide_lines(
    const std::vector<std::pair<Move, std::uint64_t>>& counts,
    std::uint64_t total_nodes, std::int64_t elapsed_ms);
[[nodiscard]] std::string format_bench_summary(
    std::uint64_t total_nodes, std::int64_t elapsed_ms);
[[nodiscard]] std::string format_speedtest_summary(
    std::size_t threads, std::size_t hash_mb,
    std::uint64_t total_nodes, std::int64_t elapsed_ms);
[[nodiscard]] std::string format_see_result(std::string_view move_uci, int gain_cp);
[[nodiscard]] std::optional<std::string> move_to_san(const GameState& state, Move move);
[[nodiscard]] std::optional<Move> parse_san_move(const GameState& state, std::string_view token);
[[nodiscard]] std::optional<Move> parse_move_uci_or_san(const GameState& state,
                                                        std::string_view token);
[[nodiscard]] std::string format_move_history(
    const GameState& origin, const std::vector<Move>& moves);
[[nodiscard]] std::string format_pgn(const GameState& origin, const std::vector<Move>& moves);
[[nodiscard]] std::string format_interactive_help();
[[nodiscard]] std::string format_position_debug_json(
    std::uint64_t generation, std::string_view command_text,
    const PositionConsistencySnapshot& snapshot, Square en_passant_square,
    std::uint64_t legal_digest);
[[nodiscard]] std::string format_hash_resize_debug_json(const HashResizeResult& result);
[[nodiscard]] std::string format_go_debug_json(
    std::uint64_t generation, const GameState& root, const SearchLimits& limits,
    std::size_t threads, bool from_book);
[[nodiscard]] std::string format_search_completion_debug_json(
    std::uint64_t generation, const GameState& search_root,
    const CompletionCandidate& candidate, const CompletionValidation& validation,
    const SearchResult& result, std::string_view source, std::string_view last_command);
[[nodiscard]] std::string format_book_completion_debug_json(
    std::uint64_t generation, const GameState& root,
    const CompletionCandidate& candidate, const CompletionValidation& validation,
    Move book_move, std::string_view last_command);
[[nodiscard]] std::string format_search_move_debug_json(
    std::uint64_t generation, const GameState& search_root,
    const std::optional<Move>& best_move, std::string_view source);
[[nodiscard]] std::string format_book_move_debug_json(
    std::uint64_t generation, const GameState& root,
    const std::optional<Move>& best_move, std::size_t book_depth);
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
parse_xboard_option_assignment(std::string_view text);
[[nodiscard]] std::optional<std::chrono::milliseconds>
parse_xboard_centiseconds_ms(std::string_view token) noexcept;
[[nodiscard]] bool is_bare_coordinate_move_token(std::string_view token) noexcept;

} // namespace koi::detail
