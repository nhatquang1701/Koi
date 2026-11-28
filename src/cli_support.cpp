#include "cli_support.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

#include "koi/detail/static_exchange.hpp"
#include "koi/detail/uci_support.hpp"
#include "koi/executable_path.hpp"
#include "koi/game_state.hpp"
#include "koi/move.hpp"
#include "koi/nnue.hpp"
#include "koi/opening_book.hpp"
#include "koi/perft.hpp"
#include "koi/search_types.hpp"
#include "koi/syzygy_tablebase.hpp"

namespace koi::cli {

namespace {

std::string json_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (const unsigned char ch : text) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(kHex[(ch >> 4U) & 0x0fU]);
                escaped.push_back(kHex[ch & 0x0fU]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

void write_engine_json(int argc, char* argv[], std::string_view version,
                       std::size_t threads, std::size_t hash_mb) {
    const std::string exe_path = resolved_executable_path(argc, argv).string();
    std::cout << "{\n"
              << "  \"type\": \"local\",\n"
              << "  \"name\": \"Koi\",\n"
              << "  \"version\": \"" << json_escape(version) << "\",\n"
              << "  \"path\": \"" << json_escape(exe_path) << "\",\n"
              << "  \"loaded\": true,\n"
              << "  \"settings\": [\n"
              << "    { \"name\": \"Threads\", \"value\": " << threads << " },\n"
              << "    { \"name\": \"Hash\", \"value\": " << hash_mb << " }\n"
              << "  ]\n"
              << "}\n";
}

void write_help(std::string_view version, std::string_view commit) {
    std::cout << "Koi " << version << " (" << commit
              << ") - C++26 UCI chess engine\n\n"
              << "Usage: koi-engine [options] [command]\n\n"
              << "Options & commands:\n"
              << "  version, -v, --version                   Print engine version and git commit\n"
              << "  help, -h, --help                         Show this help message\n"
              << "  --engine-json                            Print En Croissant / Pawn Appetit JSON profile\n"
              << "  --protocol <name>                        Start in protocol mode (uci, usi, ucci, ucicyclone, xboard)\n"
              << "  --hash <mb>                              Set initial transposition table size in MB (1..4096)\n"
              << "  --threads <n>                            Set initial search thread count (1..cores)\n"
              << "  -c, --cmd <command>                      Execute UCI/protocol command(s) in batch mode and exit (repeatable)\n"
              << "  --hce                                    Use built-in classical evaluation without a network\n"
              << "  --nnue <path>                            Load NNUE weights from path (overrides KOI_EVALUATOR)\n"
              << "  --book <path>                            Enable Polyglot opening book at path\n"
              << "  --syzygy <path>                          Configure Syzygy tablebase directory or path list\n"
              << "  bench, --bench [depth [hash [threads]]]  Run OpenBench-compatible benchmark (defaults: 10 16 1)\n"
              << "  speedtest, --speedtest [t [hash [secs]]]  Run timed multi-position NPS throughput benchmark (defaults: 1 128 3)\n"
              << "  compiler, --compiler                     Print compiler, C++26 standard, and dispatched ISA\n"
              << "  nnue [path]                              Inspect NNUE container metadata and verify payload SHA-256\n"
              << "  eval, --eval [fen]                       Evaluate startpos or FEN with the active evaluator\n"
              << "  options, --options                       Print live active protocol, evaluator, book, and Syzygy options\n"
              << "  perft <depth|--suite> [fen]              Run timed perft divide (1..10) or 6-position CPW verification suite\n"
              << "  d, --display [fen]                       Display ASCII board, FEN, Zobrist key, and move count\n"
              << "  fen [fen]                                Print six-field FEN for startpos, FEN, or moves sequence\n"
              << "  epd [fen]                                Print standard EPD (hmvc/fmvn) for startpos, FEN, or moves sequence\n"
              << "  key [fen]                                Print Zobrist position, pawn, and Polyglot hash keys\n"
              << "  moves [--san] [fen]                      List sorted legal UCI (or FIDE SAN) moves from startpos or FEN\n"
              << "  history [fen]                            Print origin, plies, UCI moves, FIDE SAN movetext, and position command\n"
              << "  pgn [fen]                                Export startpos, FEN, or moves sequence as FIDE PGN\n"
              << "  flip [fen]                               Toggle side to move if legal and print updated FEN\n"
              << "  book [fen]                               Probe Polyglot opening book entries for position\n"
              << "  tb [fen]                                 Probe Syzygy tablebases for root WDL and best moves\n"
              << "  see <move> [fen]                         Compute static exchange evaluation gain in cp\n\n"
              << "Interactive UCI commands also include: d, fen, epd, key, moves [san], book, tb, eval, nnue, flip, makemove <move...>, undo [plies], history, pgn, annotate <move|game>, see <move>, compiler, bench [depth [hash [threads]]], speedtest [threads [hash [seconds]]], options, perft <depth|suite>, wait, help\n";
}

std::optional<GameState> resolve_cli_position(const std::optional<std::string>& fen) {
    if (!fen.has_value()) {
        return GameState::startpos();
    }
    std::string error;
    const auto parsed = detail::parse_cli_position_spec(*fen, error);
    if (!parsed.has_value()) {
        std::cerr << "koi-engine: " << error << '\n';
        return std::nullopt;
    }
    return *parsed;
}

void set_environment_value(const char* name, std::string_view value) noexcept {
    std::string value_copy(value);
#if defined(_WIN32)
    (void)::_putenv_s(name, value_copy.c_str());
#else
    (void)::setenv(name, value_copy.c_str(), 1);
#endif
}

bool parse_bounded_uint(std::string_view token, std::size_t minimum, std::size_t maximum,
                        std::size_t& value_out) noexcept {
    std::size_t parsed = 0;
    const auto [end, error] =
        std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (error != std::errc{} || end != token.data() + token.size() ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    value_out = parsed;
    return true;
}

enum class ValueFlagStatus { kNotMatched, kParsed, kMissing };

ValueFlagStatus consume_value_flag(std::string_view arg, int& index, int argc, char* argv[],
                                     std::string_view long_flag, std::string_view short_flag,
                                     std::string_view& value_out) noexcept {
    if (arg == long_flag || (!short_flag.empty() && arg == short_flag)) {
        if (index + 1 >= argc || *argv[index + 1] == '\0') {
            return ValueFlagStatus::kMissing;
        }
        value_out = std::string_view(argv[++index]);
        return ValueFlagStatus::kParsed;
    }
    if (arg.starts_with(long_flag) && arg.size() > long_flag.size() &&
        arg[long_flag.size()] == '=') {
        value_out = arg.substr(long_flag.size() + 1);
        if (value_out.empty()) {
            return ValueFlagStatus::kMissing;
        }
        return ValueFlagStatus::kParsed;
    }
    return ValueFlagStatus::kNotMatched;
}

void restore_env_flag(bool& field, const char* name) noexcept {
    if (!field) {
        if (const char* env_value = std::getenv(name);
            env_value != nullptr && std::string_view(env_value) == "1") {
            field = true;
        }
    }
}

void restore_env_string(std::optional<std::string>& field, const char* name) {
    if (!field.has_value()) {
        if (const char* env_value = std::getenv(name);
            env_value != nullptr && *env_value != '\0') {
            field = env_value;
        }
    }
}

void restore_env_bounded_size(std::size_t& field, const char* name, std::size_t minimum,
                              std::size_t maximum) noexcept {
    if (const char* env_value = std::getenv(name);
        env_value != nullptr && *env_value != '\0') {
        std::size_t parsed = 0;
        if (parse_bounded_uint(env_value, minimum, maximum, parsed)) {
            field = parsed;
        }
    }
}

void restore_env_bounded_opt_size(std::optional<std::size_t>& field, const char* name,
                                  std::size_t minimum, std::size_t maximum) noexcept {
    if (!field.has_value()) {
        if (const char* env_value = std::getenv(name);
            env_value != nullptr && *env_value != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_value, minimum, maximum, parsed)) {
                field = parsed;
            }
        }
    }
}

} // namespace

std::filesystem::path resolved_executable_path(int argc, char* argv[]) {
    const std::filesystem::path module_path = current_executable_path();
    if (!module_path.empty()) {
        return module_path;
    }

    std::error_code path_error;
    if (argc > 0) {
        const std::filesystem::path path = std::filesystem::absolute(argv[0], path_error);
        if (!path_error) {
            return path;
        }
    }
    return {};
}

std::filesystem::path executable_directory(int argc, char* argv[]) {
    return resolved_executable_path(argc, argv).parent_path();
}

BootNnueResolution resolve_boot_nnue_path(
    const std::filesystem::path& engine_directory,
    const std::optional<std::string>& explicit_override) {
    if (explicit_override.has_value() && !explicit_override->empty()) {
        return {std::filesystem::path(*explicit_override), false};
    }
    if (const char* env_path = std::getenv("KOI_NNUE_PATH");
        env_path != nullptr && *env_path != '\0') {
        return {std::filesystem::path(env_path), false};
    }
    if (const auto discovered = discover_nnue_network(engine_directory); discovered.has_value()) {
        return {*discovered, true};
    }
    std::error_code working_error;
    const std::filesystem::path working_directory = std::filesystem::current_path(working_error);
    if (!working_error && working_directory != engine_directory) {
        if (const auto cwd_discovered = discover_nnue_network(working_directory);
            cwd_discovered.has_value()) {
            return {*cwd_discovered, true};
        }
    }
    return {};
}

bool hce_requested_from_environment() noexcept {
    const char* value = std::getenv("KOI_EVALUATOR");
    if (value == nullptr) {
        return false;
    }
    const std::string_view requested(value);
    const auto equals_ignore_case = [requested](std::string_view candidate) {
        if (requested.size() != candidate.size()) {
            return false;
        }
        for (std::size_t index = 0; index < requested.size(); ++index) {
            const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(requested[index])));
            const char rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(candidate[index])));
            if (lhs != rhs) {
                return false;
            }
        }
        return true;
    };
    return equals_ignore_case("hce") || equals_ignore_case("classical");
}

std::optional<int> parse_cli_options(
    int argc, char* argv[], std::string_view version, std::string_view commit,
    int default_bench_depth, std::size_t default_bench_hash_mb,
    std::size_t default_bench_threads, bool (*is_valid_protocol)(std::string_view) noexcept,
    CliOptions& out) {
    out = CliOptions{};
    out.bench_hash_mb = default_bench_hash_mb;
    out.bench_threads = default_bench_threads;

    bool cli_hce = false;
    bool cli_nnue = false;
    bool emit_engine_json = false;
    bool bench_from_cli = false;
    bool bench_hash_explicit = false;
    bool bench_threads_explicit = false;
    bool speedtest_from_cli = false;
    bool speedtest_threads_explicit = false;
    bool speedtest_hash_explicit = false;
    constexpr std::size_t kMaximumBenchHashMb = 4'096;
    const std::size_t max_bench_threads = maximum_search_threads();
    const auto collect_trailing_fen = [argc, argv](int& idx,
                                                   std::optional<std::string>& fen_out) {
        std::string collected;
        while (idx + 1 < argc) {
            const std::string_view next(argv[idx + 1]);
            // Single `-` is the empty en-passant square in an unquoted FEN, whereas
            // longer leading-dash tokens (`-v`, `--help`, `--protocol`) remain flags.
            if (next != "-" && next.starts_with('-')) {
                break;
            }
            if (!collected.empty()) {
                collected.push_back(' ');
            }
            collected.append(argv[++idx]);
        }
        if (!collected.empty()) {
            fen_out = std::move(collected);
        }
    };

    const auto append_cli_command = [&out](std::string_view cmd) {
        out.commands.emplace_back(cmd);
        std::string joined;
        for (const std::string& command : out.commands) {
            if (!joined.empty()) {
                joined.push_back('\n');
            }
            joined.append(command);
        }
        set_environment_value("KOI_CLI_COMMANDS", joined);
    };
    const auto set_inspect_command = [&](int& arg_index, bool& flag, const char* env_name) {
        flag = true;
        set_environment_value(env_name, "1");
        collect_trailing_fen(arg_index, out.inspect_fen);
        if (out.inspect_fen.has_value()) {
            set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
        }
    };

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "version" || arg == "--version" || arg == "-v") {
            std::cout << "Koi " << version << " (" << commit << ")\n";
            return 0;
        }
        if (arg == "help" || arg == "--help" || arg == "-h") {
            write_help(version, commit);
            return 0;
        }
        if (arg == "--engine-json") {
            emit_engine_json = true;
            continue;
        }
        if (arg == "compiler" || arg == "--compiler") {
            out.compiler = true;
            set_environment_value("KOI_CLI_COMPILER", "1");
            continue;
        }
        if (arg == "nnue") {
            out.nnue_info = true;
            set_environment_value("KOI_CLI_NNUE_INFO", "1");
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                out.nnue_info_path = argv[++index];
                set_environment_value("KOI_CLI_NNUE_INFO_PATH", *out.nnue_info_path);
            }
            continue;
        }
        if (arg == "d" || arg == "--display") {
            set_inspect_command(index, out.display, "KOI_CLI_DISPLAY");
            continue;
        }
        if (arg == "fen") {
            set_inspect_command(index, out.fen, "KOI_CLI_FEN_CMD");
            continue;
        }
        if (arg == "epd") {
            set_inspect_command(index, out.epd, "KOI_CLI_EPD");
            continue;
        }
        if (arg == "key") {
            set_inspect_command(index, out.key, "KOI_CLI_KEY");
            continue;
        }
        if (arg == "moves") {
            if (index + 1 < argc) {
                const std::string_view next_arg(argv[index + 1]);
                if (next_arg == "--san" || next_arg == "san") {
                    out.moves_san = true;
                    set_environment_value("KOI_CLI_MOVES_SAN", "1");
                    ++index;
                }
            }
            set_inspect_command(index, out.moves, "KOI_CLI_MOVES");
            continue;
        }
        if (arg == "history") {
            set_inspect_command(index, out.history, "KOI_CLI_HISTORY");
            continue;
        }
        if (arg == "pgn") {
            set_inspect_command(index, out.pgn, "KOI_CLI_PGN");
            continue;
        }
        if (arg == "flip") {
            set_inspect_command(index, out.flip, "KOI_CLI_FLIP");
            continue;
        }
        if (arg == "book") {
            set_inspect_command(index, out.book, "KOI_CLI_BOOK");
            continue;
        }
        if (arg == "tb") {
            set_inspect_command(index, out.tb, "KOI_CLI_TB");
            continue;
        }
        if (arg == "see") {
            if (index + 1 >= argc) {
                std::cerr << "koi-engine: missing move for see (e.g. see e4d5 [fen]).\n";
                return 2;
            }
            out.see_move = std::string(argv[++index]);
            set_environment_value("KOI_CLI_SEE_MOVE", *out.see_move);
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
            continue;
        }
        if (arg == "eval" || arg == "--eval") {
            out.eval = true;
            set_environment_value("KOI_CLI_EVAL", "1");
            collect_trailing_fen(index, out.eval_fen);
            if (out.eval_fen.has_value()) {
                set_environment_value("KOI_CLI_EVAL_FEN", *out.eval_fen);
            }
            continue;
        }
        if (arg == "options" || arg == "--options") {
            out.options = true;
            set_environment_value("KOI_CLI_OPTIONS", "1");
            continue;
        }
        if (arg == "perft") {
            if (index + 1 >= argc) {
                std::cerr << "koi-engine: missing depth for perft (expected integer 1..10 or --suite).\n";
                return 2;
            }
            const std::string_view depth_arg(argv[++index]);
            if (depth_arg == "--suite" || depth_arg == "suite") {
                if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                    std::cerr << "koi-engine: perft --suite does not accept a position argument.\n";
                    return 2;
                }
                out.perft_suite = true;
                set_environment_value("KOI_CLI_PERFT_SUITE", "1");
                continue;
            }
            std::size_t depth = 0;
            if (!parse_bounded_uint(depth_arg, 1, 10, depth)) {
                std::cerr << "koi-engine: invalid perft depth '" << depth_arg
                          << "' (expected integer 1..10).\n";
                return 2;
            }
            out.perft_depth = static_cast<int>(depth);
            set_environment_value("KOI_CLI_PERFT", std::to_string(depth));
            collect_trailing_fen(index, out.perft_fen);
            if (out.perft_fen.has_value()) {
                set_environment_value("KOI_CLI_PERFT_FEN", *out.perft_fen);
            }
            continue;
        }
        if (arg == "bench" || arg == "--bench") {
            bench_from_cli = true;
            std::size_t depth = static_cast<std::size_t>(default_bench_depth);
            std::size_t hash_mb = default_bench_hash_mb;
            std::size_t threads = default_bench_threads;
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, 64, depth)) {
                    std::cerr << "koi-engine: invalid bench depth '" << next_arg
                              << "' (expected integer 1..64).\n";
                    return 2;
                }
            }
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, kMaximumBenchHashMb, hash_mb)) {
                    std::cerr << "koi-engine: invalid bench hash '" << next_arg
                              << "' (expected integer 1.." << kMaximumBenchHashMb << ").\n";
                    return 2;
                }
                bench_hash_explicit = true;
            }
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, max_bench_threads, threads)) {
                    std::cerr << "koi-engine: invalid bench threads '" << next_arg
                              << "' (expected integer 1.." << max_bench_threads << ").\n";
                    return 2;
                }
                bench_threads_explicit = true;
            }
            out.bench_depth = static_cast<int>(depth);
            out.bench_hash_mb = hash_mb;
            out.bench_threads = threads;
            set_environment_value("KOI_BENCH_DEPTH", std::to_string(depth));
            set_environment_value("KOI_BENCH_HASH", std::to_string(hash_mb));
            set_environment_value("KOI_BENCH_THREADS", std::to_string(threads));
            continue;
        }
        if (arg == "speedtest" || arg == "--speedtest") {
            speedtest_from_cli = true;
            std::size_t threads = out.speedtest_threads;
            std::size_t hash_mb = out.speedtest_hash_mb;
            std::size_t seconds = static_cast<std::size_t>(out.speedtest_seconds);
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, max_bench_threads, threads)) {
                    std::cerr << "koi-engine: invalid speedtest threads '" << next_arg
                              << "' (expected integer 1.." << max_bench_threads << ").\n";
                    return 2;
                }
                speedtest_threads_explicit = true;
            }
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, kMaximumBenchHashMb, hash_mb)) {
                    std::cerr << "koi-engine: invalid speedtest hash '" << next_arg
                              << "' (expected integer 1.." << kMaximumBenchHashMb << ").\n";
                    return 2;
                }
                speedtest_hash_explicit = true;
            }
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, 60, seconds)) {
                    std::cerr << "koi-engine: invalid speedtest seconds '" << next_arg
                              << "' (expected integer 1..60).\n";
                    return 2;
                }
            }
            out.speedtest = true;
            out.speedtest_threads = threads;
            out.speedtest_hash_mb = hash_mb;
            out.speedtest_seconds = static_cast<int>(seconds);
            set_environment_value("KOI_SPEEDTEST", "1");
            set_environment_value("KOI_SPEEDTEST_THREADS", std::to_string(threads));
            set_environment_value("KOI_SPEEDTEST_HASH", std::to_string(hash_mb));
            set_environment_value("KOI_SPEEDTEST_SECONDS", std::to_string(seconds));
            continue;
        }
        if (arg == "--hce") {
            cli_hce = true;
            set_environment_value("KOI_EVALUATOR", "hce");
            continue;
        }
        std::string_view flag_value;
        if (const auto status =
                consume_value_flag(arg, index, argc, argv, "--nnue", "", flag_value);
            status != ValueFlagStatus::kNotMatched) {
            if (status == ValueFlagStatus::kMissing) {
                std::cerr << "koi-engine: missing path for --nnue.\n";
                return 2;
            }
            cli_nnue = true;
            set_environment_value("KOI_NNUE_PATH", flag_value);
            set_environment_value("KOI_EVALUATOR", "nnue");
            continue;
        }
        if (const auto status =
                consume_value_flag(arg, index, argc, argv, "--book", "", flag_value);
            status != ValueFlagStatus::kNotMatched) {
            if (status == ValueFlagStatus::kMissing) {
                std::cerr << "koi-engine: missing path for --book.\n";
                return 2;
            }
            out.book_path = std::string(flag_value);
            set_environment_value("KOI_CLI_BOOK_PATH", *out.book_path);
            continue;
        }
        if (const auto status =
                consume_value_flag(arg, index, argc, argv, "--syzygy", "", flag_value);
            status != ValueFlagStatus::kNotMatched) {
            if (status == ValueFlagStatus::kMissing) {
                std::cerr << "koi-engine: missing path for --syzygy.\n";
                return 2;
            }
            out.syzygy_path = std::string(flag_value);
            set_environment_value("KOI_CLI_SYZYGY_PATH", *out.syzygy_path);
            continue;
        }
        if (const auto status =
                consume_value_flag(arg, index, argc, argv, "--cmd", "-c", flag_value);
            status != ValueFlagStatus::kNotMatched) {
            if (status == ValueFlagStatus::kMissing) {
                std::cerr << "koi-engine: missing command for --cmd.\n";
                return 2;
            }
            append_cli_command(flag_value);
            continue;
        }
        if (const auto status =
                consume_value_flag(arg, index, argc, argv, "--protocol", "", flag_value);
            status != ValueFlagStatus::kNotMatched) {
            if (status == ValueFlagStatus::kMissing) {
                std::cerr << "koi-engine: missing argument for --protocol (expected uci, usi, "
                             "ucci, ucicyclone, or xboard).\n";
                return 2;
            }
            if (!is_valid_protocol(flag_value)) {
                std::cerr << "koi-engine: unknown protocol '" << flag_value
                          << "' (supported: uci, usi, ucci, ucicyclone, xboard).\n";
                return 2;
            }
            out.initial_protocol = std::string(flag_value);
            set_environment_value("KOI_PROTOCOL", flag_value);
            continue;
        }
        if (arg == "--hash" || arg.starts_with("--hash=") ||
            arg == "--threads" || arg.starts_with("--threads=")) {
            const bool is_hash = arg == "--hash" || arg.starts_with("--hash=");
            const std::string_view flag = is_hash ? "--hash" : "--threads";
            if (consume_value_flag(arg, index, argc, argv, flag, "", flag_value) ==
                ValueFlagStatus::kMissing) {
                std::cerr << "koi-engine: missing value for " << flag << ".\n";
                return 2;
            }
            const std::size_t limit = is_hash ? kMaximumBenchHashMb : max_bench_threads;
            std::size_t parsed = 0;
            if (!parse_bounded_uint(flag_value, 1, limit, parsed)) {
                std::cerr << "koi-engine: invalid " << (is_hash ? "hash" : "threads") << " '"
                          << flag_value << "' (expected integer 1.." << limit << ").\n";
                return 2;
            }
            if (is_hash) {
                out.initial_hash_mb = parsed;
                set_environment_value("KOI_CLI_HASH", std::to_string(parsed));
            } else {
                out.initial_threads = parsed;
                set_environment_value("KOI_CLI_THREADS", std::to_string(parsed));
            }
            continue;
        }
        std::cerr << "koi-engine: unknown option '" << arg
                  << "' (supported: version/--version, help/--help, --engine-json, "
                     "--protocol <name>, --hash <mb>, --threads <n>, -c/--cmd <command>, "
                     "--hce, --nnue <path>, --book <path>, --syzygy <path>, "
                     "bench [depth [hash [threads]]], speedtest [threads [hash [seconds]]], "
                     "compiler, nnue [path], eval [fen], options, perft <depth|--suite> [fen], "
                     "d [fen], fen [fen], epd [fen], key [fen], moves [--san] [fen], "
                     "history [fen], pgn [fen], flip [fen], book [fen], "
                     "tb [fen], see <move> [fen]).\n";
        return 2;
    }

    if (cli_hce && cli_nnue) {
        std::cerr << "koi-engine: --hce and --nnue cannot be combined.\n";
        return 2;
    }

    if (!out.initial_protocol.has_value()) {
        if (const char* env_protocol = std::getenv("KOI_PROTOCOL");
            env_protocol != nullptr && *env_protocol != '\0' &&
            is_valid_protocol(env_protocol)) {
            out.initial_protocol = env_protocol;
        }
    }
    restore_env_bounded_opt_size(out.initial_hash_mb, "KOI_CLI_HASH", 1, kMaximumBenchHashMb);
    restore_env_bounded_opt_size(out.initial_threads, "KOI_CLI_THREADS", 1, max_bench_threads);
    if (emit_engine_json) {
        write_engine_json(argc, argv, version, out.initial_threads.value_or(1),
                          out.initial_hash_mb.value_or(512));
        return 0;
    }
    if (bench_from_cli) {
        if (!bench_hash_explicit && out.initial_hash_mb.has_value()) {
            out.bench_hash_mb = *out.initial_hash_mb;
            set_environment_value("KOI_BENCH_HASH", std::to_string(out.bench_hash_mb));
        }
        if (!bench_threads_explicit && out.initial_threads.has_value()) {
            out.bench_threads = *out.initial_threads;
            set_environment_value("KOI_BENCH_THREADS", std::to_string(out.bench_threads));
        }
    }
    if (speedtest_from_cli) {
        if (!speedtest_threads_explicit && out.initial_threads.has_value()) {
            out.speedtest_threads = *out.initial_threads;
            set_environment_value("KOI_SPEEDTEST_THREADS",
                                  std::to_string(out.speedtest_threads));
        }
        if (!speedtest_hash_explicit && out.initial_hash_mb.has_value()) {
            out.speedtest_hash_mb = *out.initial_hash_mb;
            set_environment_value("KOI_SPEEDTEST_HASH", std::to_string(out.speedtest_hash_mb));
        }
    }
    if (out.commands.empty()) {
        if (const char* env_commands = std::getenv("KOI_CLI_COMMANDS");
            env_commands != nullptr && *env_commands != '\0') {
            std::string_view remaining(env_commands);
            while (!remaining.empty()) {
                const std::size_t newline = remaining.find('\n');
                const std::string_view segment =
                    newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
                if (!segment.empty()) {
                    out.commands.emplace_back(segment);
                }
                if (newline == std::string_view::npos) {
                    break;
                }
                remaining.remove_prefix(newline + 1);
            }
        }
    }
    restore_env_flag(out.compiler, "KOI_CLI_COMPILER");
    restore_env_flag(out.display, "KOI_CLI_DISPLAY");
    restore_env_flag(out.fen, "KOI_CLI_FEN_CMD");
    restore_env_flag(out.epd, "KOI_CLI_EPD");
    restore_env_flag(out.key, "KOI_CLI_KEY");
    restore_env_flag(out.moves, "KOI_CLI_MOVES");
    restore_env_flag(out.moves_san, "KOI_CLI_MOVES_SAN");
    restore_env_flag(out.history, "KOI_CLI_HISTORY");
    restore_env_flag(out.pgn, "KOI_CLI_PGN");
    restore_env_flag(out.flip, "KOI_CLI_FLIP");
    restore_env_flag(out.book, "KOI_CLI_BOOK");
    restore_env_flag(out.tb, "KOI_CLI_TB");
    restore_env_flag(out.nnue_info, "KOI_CLI_NNUE_INFO");
    restore_env_string(out.nnue_info_path, "KOI_CLI_NNUE_INFO_PATH");
    restore_env_string(out.book_path, "KOI_CLI_BOOK_PATH");
    restore_env_string(out.syzygy_path, "KOI_CLI_SYZYGY_PATH");
    restore_env_string(out.see_move, "KOI_CLI_SEE_MOVE");
    restore_env_string(out.inspect_fen, "KOI_CLI_FEN");
    restore_env_flag(out.eval, "KOI_CLI_EVAL");
    restore_env_flag(out.options, "KOI_CLI_OPTIONS");
    restore_env_string(out.eval_fen, "KOI_CLI_EVAL_FEN");
    if (!out.perft_depth.has_value()) {
        std::optional<std::size_t> parsed;
        restore_env_bounded_opt_size(parsed, "KOI_CLI_PERFT", 1, 10);
        if (parsed.has_value()) {
            out.perft_depth = static_cast<int>(*parsed);
        }
    }
    restore_env_flag(out.perft_suite, "KOI_CLI_PERFT_SUITE");
    restore_env_string(out.perft_fen, "KOI_CLI_PERFT_FEN");
    if (!out.bench_depth.has_value()) {
        std::optional<std::size_t> parsed_depth;
        restore_env_bounded_opt_size(parsed_depth, "KOI_BENCH_DEPTH", 1, 64);
        if (parsed_depth.has_value()) {
            out.bench_depth = static_cast<int>(*parsed_depth);
        }
        restore_env_bounded_size(out.bench_hash_mb, "KOI_BENCH_HASH", 1, kMaximumBenchHashMb);
        restore_env_bounded_size(out.bench_threads, "KOI_BENCH_THREADS", 1, max_bench_threads);
    }
    if (!out.speedtest) {
        restore_env_flag(out.speedtest, "KOI_SPEEDTEST");
        restore_env_bounded_size(out.speedtest_threads, "KOI_SPEEDTEST_THREADS", 1,
                                 max_bench_threads);
        restore_env_bounded_size(out.speedtest_hash_mb, "KOI_SPEEDTEST_HASH", 1,
                                 kMaximumBenchHashMb);
        std::optional<std::size_t> parsed_seconds;
        restore_env_bounded_opt_size(parsed_seconds, "KOI_SPEEDTEST_SECONDS", 1, 60);
        if (parsed_seconds.has_value()) {
            out.speedtest_seconds = static_cast<int>(*parsed_seconds);
        }
    }
    if (!out.commands.empty() &&
        (out.bench_depth.has_value() || out.speedtest || out.compiler || out.nnue_info ||
         out.display || out.fen || out.epd || out.key || out.moves || out.history || out.pgn ||
         out.flip || out.book || out.tb || out.see_move.has_value() || out.eval || out.options ||
         out.perft_depth.has_value() || out.perft_suite)) {
        std::cerr << "koi-engine: --cmd cannot be combined with one-shot subcommands.\n";
        return 2;
    }
    return std::nullopt;
}

std::optional<int> run_zero_network_cli_command(
    const CliOptions& options, int argc, char* argv[], std::string_view version,
    std::string_view commit) {
    if (options.compiler) {
        std::cout << detail::format_compiler_info(version, commit) << std::flush;
        return 0;
    }
    if (options.nnue_info) {
        const std::filesystem::path engine_directory = executable_directory(argc, argv);
        const std::filesystem::path target_path =
            resolve_boot_nnue_path(engine_directory, options.nnue_info_path).path;
        if (target_path.empty()) {
            std::cerr << "koi-engine: no NNUE network found to inspect "
                         "(pass a path or set KOI_NNUE_PATH).\n";
            return 2;
        }
        const auto loaded = NnueLoader::load_file(target_path);
        if (!loaded.has_value()) {
            std::cerr << "koi-engine: invalid NNUE network '" << target_path.string() << "' ("
                      << loaded.error().message << ").\n";
            return 2;
        }
        const std::string display_name = target_path.filename().empty()
            ? target_path.string()
            : target_path.filename().string();
        std::cout << detail::format_nnue_info(display_name, *loaded) << std::flush;
        return 0;
    }
    if (options.pgn || options.history) {
        detail::ParsedCliPosition parsed{GameState::startpos(), {}, GameState::startpos()};
        if (options.inspect_fen.has_value()) {
            std::string error;
            const auto resolved =
                detail::parse_cli_position_with_history(*options.inspect_fen, error);
            if (!resolved.has_value()) {
                std::cerr << "koi-engine: " << error << '\n';
                return 2;
            }
            parsed = *resolved;
        }
        std::cout << (options.pgn ? detail::format_pgn(parsed.origin, parsed.moves)
                                  : detail::format_move_history(parsed.origin, parsed.moves))
                  << std::flush;
        return 0;
    }
    if (options.display || options.fen || options.epd || options.key || options.moves ||
        options.flip) {
        const std::optional<GameState> root = resolve_cli_position(options.inspect_fen);
        if (!root.has_value()) {
            return 2;
        }
        if (options.flip) {
            if (root->in_check()) {
                std::cerr << "koi-engine: flip rejected: position cannot legally switch sides.\n";
                return 2;
            }
            const std::optional<std::string> flipped = detail::flip_position_fen(root->fen());
            if (!flipped.has_value() || !GameState::from_fen(*flipped).has_value()) {
                std::cerr << "koi-engine: flip rejected: position cannot legally switch sides.\n";
                return 2;
            }
            std::cout << *flipped << '\n' << std::flush;
            return 0;
        }
        if (options.display) {
            std::cout << detail::format_ascii_board(*root) << std::flush;
        } else if (options.fen) {
            std::cout << root->fen() << '\n' << std::flush;
        } else if (options.epd) {
            std::cout << detail::format_epd(*root) << std::flush;
        } else if (options.key) {
            std::cout << detail::format_key_info(*root) << std::flush;
        } else {
            std::cout << (options.moves_san ? detail::format_legal_moves_san(*root)
                                            : detail::format_legal_moves(*root))
                      << std::flush;
        }
        return 0;
    }
    if (options.see_move.has_value()) {
        const std::optional<GameState> root = resolve_cli_position(options.inspect_fen);
        if (!root.has_value()) {
            return 2;
        }
        const std::optional<Move> move = detail::parse_move_uci_or_san(*root, *options.see_move);
        if (!move.has_value() || !root->is_legal(*move)) {
            std::cerr << "koi-engine: illegal move '" << *options.see_move << "' for see.\n";
            return 2;
        }
        const int gain = detail::static_exchange_gain(*root, *move);
        std::cout << detail::format_see_result(move->uci(), gain) << std::flush;
        return 0;
    }
    if (options.book) {
        const std::optional<GameState> root = resolve_cli_position(options.inspect_fen);
        if (!root.has_value()) {
            return 2;
        }
        const std::filesystem::path engine_directory = executable_directory(argc, argv);
        std::filesystem::path book_path;
        if (options.book_path.has_value()) {
            book_path = std::filesystem::path(*options.book_path);
        } else {
            const std::filesystem::path primary = engine_directory / "book.bin";
            const std::filesystem::path packaged = engine_directory / "book" / "book.bin";
            const std::filesystem::path bundled = engine_directory / "book" / "koi.bin";
            if (OpeningBook::is_usable_file(primary)) {
                book_path = primary;
            } else if (OpeningBook::is_usable_file(packaged)) {
                book_path = packaged;
            } else {
                book_path = bundled;
            }
        }
        if (OpeningBook::is_usable_file(book_path) &&
            static_cast<bool>(std::ifstream(book_path, std::ios::binary))) {
            OpeningBook book(engine_directory);
            book.set_file(book_path);
            std::cout << detail::format_book_entries(book_path.filename().string(),
                                                     book.probe_all(*root))
                      << std::flush;
        } else {
            std::cout << "Opening book : unavailable\n" << std::flush;
        }
        return 0;
    }
    if (options.tb) {
        const std::optional<GameState> root = resolve_cli_position(options.inspect_fen);
        if (!root.has_value()) {
            return 2;
        }
        const std::filesystem::path engine_directory = executable_directory(argc, argv);
        const std::filesystem::path tb_path = options.syzygy_path.has_value()
            ? std::filesystem::path(*options.syzygy_path)
            : discover_syzygy_directory(engine_directory).value_or(std::filesystem::path{});
        if (!tb_path.empty() && is_usable_syzygy_directory(tb_path)) {
            SyzygyTablebase tb(tb_path);
            std::cout << detail::format_syzygy_probe(&tb, *root) << std::flush;
        } else {
            std::cout << detail::format_syzygy_probe(nullptr, *root) << std::flush;
        }
        return 0;
    }
    if (options.perft_suite) {
        const detail::PerftSuiteReport report = detail::run_perft_suite();
        for (const std::string& line : report.lines) {
            std::cout << line << '\n';
        }
        std::cout << std::flush;
        return report.passed == report.total ? 0 : 1;
    }
    if (options.perft_depth.has_value()) {
        const std::optional<GameState> resolved = resolve_cli_position(options.perft_fen);
        if (!resolved.has_value()) {
            return 2;
        }
        const GameState& root = *resolved;
        std::array<Move, kMaximumLegalMoves> legal_moves{};
        const std::size_t move_count =
            std::min(root.legal_moves_into(legal_moves), legal_moves.size());
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t total = 0;
        std::vector<std::pair<Move, std::uint64_t>> counts;
        counts.reserve(move_count);
        for (std::size_t index = 0; index < move_count; ++index) {
            const Move& move = legal_moves[index];
            GameState child = root;
            if (!child.make_move(move)) {
                continue;
            }
            const std::uint64_t nodes = perft(child, *options.perft_depth - 1);
            total += nodes;
            counts.emplace_back(move, nodes);
        }
        const auto elapsed_ms = std::max<std::int64_t>(
            1, std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
                   .count());
        for (const std::string& line :
             detail::format_perft_divide_lines(counts, total, elapsed_ms)) {
            std::cout << "info string " << line << '\n';
        }
        std::cout << std::flush;
        return 0;
    }
    return std::nullopt;
}

std::string build_command_batch_script(const std::vector<std::string>& commands) {
    const auto is_space = [](const char ch) noexcept {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
    };
    std::string script;
    std::string_view last_line;
    for (const std::string& command : commands) {
        std::string_view remaining(command);
        while (!remaining.empty()) {
            const std::size_t newline = remaining.find('\n');
            std::string_view line =
                newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            std::string_view trimmed = line;
            while (!trimmed.empty() && is_space(trimmed.front())) {
                trimmed.remove_prefix(1);
            }
            if (!trimmed.empty()) {
                script.append(line);
                script.push_back('\n');
                last_line = trimmed;
            }
            if (newline == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(newline + 1);
        }
    }
    std::size_t token_end = 0;
    while (token_end < last_line.size() && !is_space(last_line[token_end])) {
        ++token_end;
    }
    const std::string_view last_token = last_line.substr(0, token_end);
    if (last_token != "quit" && last_token != "bye") {
        script += "wait\nquit\n";
    }
    return script;
}

} // namespace koi::cli
