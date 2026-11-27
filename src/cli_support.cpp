#include "cli_support.hpp"

#include <algorithm>
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

void write_engine_json(int argc, char* argv[], std::string_view version) {
    const std::string exe_path = resolved_executable_path(argc, argv).string();
    std::cout << "{\n"
              << "  \"type\": \"local\",\n"
              << "  \"name\": \"Koi\",\n"
              << "  \"version\": \"" << json_escape(version) << "\",\n"
              << "  \"path\": \"" << json_escape(exe_path) << "\",\n"
              << "  \"loaded\": true,\n"
              << "  \"settings\": [\n"
              << "    { \"name\": \"Threads\", \"value\": 1 },\n"
              << "    { \"name\": \"Hash\", \"value\": 512 }\n"
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
              << "  -c, --cmd <command>                      Execute UCI/protocol command(s) in batch mode and exit (repeatable)\n"
              << "  --hce                                    Use built-in classical evaluation without a network\n"
              << "  --nnue <path>                            Load NNUE weights from path (overrides KOI_EVALUATOR)\n"
              << "  --book <path>                            Specify Polyglot book path for one-shot book\n"
              << "  --syzygy <path>                          Specify Syzygy directory for one-shot tb\n"
              << "  bench, --bench [depth [hash [threads]]]  Run OpenBench-compatible benchmark (defaults: 10 16 1)\n"
              << "  speedtest, --speedtest [t [hash [secs]]]  Run timed multi-position NPS throughput benchmark (defaults: 1 128 3)\n"
              << "  compiler, --compiler                     Print compiler, C++26 standard, and dispatched ISA\n"
              << "  nnue [path]                              Inspect NNUE container metadata and verify payload SHA-256\n"
              << "  eval, --eval [fen]                       Evaluate startpos or FEN with the active evaluator\n"
              << "  options, --options                       Print live active protocol, evaluator, book, and Syzygy options\n"
              << "  perft <depth> [fen]                      Run timed perft divide (depth 1..10)\n"
              << "  d, --display [fen]                       Display ASCII board, FEN, Zobrist key, and move count\n"
              << "  fen [fen]                                Print six-field FEN for startpos, FEN, or moves sequence\n"
              << "  key [fen]                                Print Zobrist position, pawn, and Polyglot hash keys\n"
              << "  moves [fen]                              List sorted legal UCI moves from startpos or FEN\n"
              << "  flip [fen]                               Toggle side to move if legal and print updated FEN\n"
              << "  book [fen]                               Probe Polyglot opening book entries for position\n"
              << "  tb [fen]                                 Probe Syzygy tablebases for root WDL and best moves\n"
              << "  see <move> [fen]                         Compute static exchange evaluation gain in cp\n\n"
              << "Interactive UCI commands also include: d, fen, key, moves, book, tb, eval, nnue, flip, makemove <uci...>, undo [plies], see <move>, compiler, bench [depth [hash [threads]]], speedtest [threads [hash [seconds]]], options, perft <depth>, wait, help\n";
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
            write_engine_json(argc, argv, version);
            return 0;
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
            out.display = true;
            set_environment_value("KOI_CLI_DISPLAY", "1");
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
            continue;
        }
        if (arg == "fen") {
            out.fen = true;
            set_environment_value("KOI_CLI_FEN_CMD", "1");
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
            continue;
        }
        if (arg == "key") {
            out.key = true;
            set_environment_value("KOI_CLI_KEY", "1");
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
            continue;
        }
        if (arg == "moves") {
            out.moves = true;
            set_environment_value("KOI_CLI_MOVES", "1");
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
            continue;
        }
        if (arg == "flip") {
            out.flip = true;
            set_environment_value("KOI_CLI_FLIP", "1");
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
            continue;
        }
        if (arg == "book") {
            out.book = true;
            set_environment_value("KOI_CLI_BOOK", "1");
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
            continue;
        }
        if (arg == "tb") {
            out.tb = true;
            set_environment_value("KOI_CLI_TB", "1");
            collect_trailing_fen(index, out.inspect_fen);
            if (out.inspect_fen.has_value()) {
                set_environment_value("KOI_CLI_FEN", *out.inspect_fen);
            }
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
                std::cerr << "koi-engine: missing depth for perft (expected integer 1..10).\n";
                return 2;
            }
            const std::string_view depth_arg(argv[++index]);
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
            }
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, max_bench_threads, threads)) {
                    std::cerr << "koi-engine: invalid bench threads '" << next_arg
                              << "' (expected integer 1.." << max_bench_threads << ").\n";
                    return 2;
                }
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
            }
            if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with('-')) {
                const std::string_view next_arg(argv[++index]);
                if (!parse_bounded_uint(next_arg, 1, kMaximumBenchHashMb, hash_mb)) {
                    std::cerr << "koi-engine: invalid speedtest hash '" << next_arg
                              << "' (expected integer 1.." << kMaximumBenchHashMb << ").\n";
                    return 2;
                }
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
        if (arg == "--nnue") {
            if (index + 1 >= argc || *argv[index + 1] == '\0') {
                std::cerr << "koi-engine: missing path for --nnue.\n";
                return 2;
            }
            cli_nnue = true;
            set_environment_value("KOI_NNUE_PATH", argv[++index]);
            set_environment_value("KOI_EVALUATOR", "nnue");
            continue;
        }
        if (arg.starts_with("--nnue=")) {
            const std::string_view nnue_arg = arg.substr(std::string_view("--nnue=").size());
            if (nnue_arg.empty()) {
                std::cerr << "koi-engine: missing path for --nnue.\n";
                return 2;
            }
            cli_nnue = true;
            set_environment_value("KOI_NNUE_PATH", nnue_arg);
            set_environment_value("KOI_EVALUATOR", "nnue");
            continue;
        }
        if (arg == "--book") {
            if (index + 1 >= argc || *argv[index + 1] == '\0') {
                std::cerr << "koi-engine: missing path for --book.\n";
                return 2;
            }
            out.book_path = argv[++index];
            set_environment_value("KOI_CLI_BOOK_PATH", *out.book_path);
            continue;
        }
        if (arg.starts_with("--book=")) {
            const std::string_view book_arg = arg.substr(std::string_view("--book=").size());
            if (book_arg.empty()) {
                std::cerr << "koi-engine: missing path for --book.\n";
                return 2;
            }
            out.book_path = std::string(book_arg);
            set_environment_value("KOI_CLI_BOOK_PATH", *out.book_path);
            continue;
        }
        if (arg == "--syzygy") {
            if (index + 1 >= argc || *argv[index + 1] == '\0') {
                std::cerr << "koi-engine: missing path for --syzygy.\n";
                return 2;
            }
            out.syzygy_path = argv[++index];
            set_environment_value("KOI_CLI_SYZYGY_PATH", *out.syzygy_path);
            continue;
        }
        if (arg.starts_with("--syzygy=")) {
            const std::string_view syzygy_arg = arg.substr(std::string_view("--syzygy=").size());
            if (syzygy_arg.empty()) {
                std::cerr << "koi-engine: missing path for --syzygy.\n";
                return 2;
            }
            out.syzygy_path = std::string(syzygy_arg);
            set_environment_value("KOI_CLI_SYZYGY_PATH", *out.syzygy_path);
            continue;
        }
        if (arg == "-c" || arg == "--cmd") {
            if (index + 1 >= argc || *argv[index + 1] == '\0') {
                std::cerr << "koi-engine: missing command for --cmd.\n";
                return 2;
            }
            append_cli_command(argv[++index]);
            continue;
        }
        if (arg.starts_with("--cmd=")) {
            const std::string_view cmd_arg = arg.substr(std::string_view("--cmd=").size());
            if (cmd_arg.empty()) {
                std::cerr << "koi-engine: missing command for --cmd.\n";
                return 2;
            }
            append_cli_command(cmd_arg);
            continue;
        }
        std::optional<std::string_view> parsed_protocol;
        if (arg == "--protocol") {
            if (index + 1 >= argc) {
                std::cerr << "koi-engine: missing argument for --protocol (expected uci, usi, "
                             "ucci, ucicyclone, or xboard).\n";
                return 2;
            }
            parsed_protocol = std::string_view(argv[++index]);
        } else if (arg.starts_with("--protocol=")) {
            parsed_protocol = arg.substr(std::string_view("--protocol=").size());
        } else {
            std::cerr << "koi-engine: unknown option '" << arg
                      << "' (supported: version/--version, help/--help, --engine-json, "
                         "--protocol <name>, -c/--cmd <command>, --hce, --nnue <path>, "
                         "--book <path>, --syzygy <path>, bench [depth [hash [threads]]], "
                         "speedtest [threads [hash [seconds]]], compiler, nnue [path], eval [fen], "
                         "options, perft <depth> [fen], d [fen], fen [fen], key [fen], moves [fen], flip [fen], "
                         "book [fen], tb [fen], see <move> [fen]).\n";
            return 2;
        }
        if (parsed_protocol.has_value()) {
            if (!is_valid_protocol(*parsed_protocol)) {
                std::cerr << "koi-engine: unknown protocol '" << *parsed_protocol
                          << "' (supported: uci, usi, ucci, ucicyclone, xboard).\n";
                return 2;
            }
            out.initial_protocol = std::string(*parsed_protocol);
            set_environment_value("KOI_PROTOCOL", *parsed_protocol);
        }
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
    if (!out.compiler) {
        if (const char* env_compiler = std::getenv("KOI_CLI_COMPILER");
            env_compiler != nullptr && std::string_view(env_compiler) == "1") {
            out.compiler = true;
        }
    }
    if (!out.display) {
        if (const char* env_display = std::getenv("KOI_CLI_DISPLAY");
            env_display != nullptr && std::string_view(env_display) == "1") {
            out.display = true;
        }
    }
    if (!out.fen) {
        if (const char* env_fen_cmd = std::getenv("KOI_CLI_FEN_CMD");
            env_fen_cmd != nullptr && std::string_view(env_fen_cmd) == "1") {
            out.fen = true;
        }
    }
    if (!out.key) {
        if (const char* env_key = std::getenv("KOI_CLI_KEY");
            env_key != nullptr && std::string_view(env_key) == "1") {
            out.key = true;
        }
    }
    if (!out.moves) {
        if (const char* env_moves = std::getenv("KOI_CLI_MOVES");
            env_moves != nullptr && std::string_view(env_moves) == "1") {
            out.moves = true;
        }
    }
    if (!out.flip) {
        if (const char* env_flip = std::getenv("KOI_CLI_FLIP");
            env_flip != nullptr && std::string_view(env_flip) == "1") {
            out.flip = true;
        }
    }
    if (!out.book) {
        if (const char* env_book = std::getenv("KOI_CLI_BOOK");
            env_book != nullptr && std::string_view(env_book) == "1") {
            out.book = true;
        }
    }
    if (!out.tb) {
        if (const char* env_tb = std::getenv("KOI_CLI_TB");
            env_tb != nullptr && std::string_view(env_tb) == "1") {
            out.tb = true;
        }
    }
    if (!out.nnue_info) {
        if (const char* env_nnue_info = std::getenv("KOI_CLI_NNUE_INFO");
            env_nnue_info != nullptr && std::string_view(env_nnue_info) == "1") {
            out.nnue_info = true;
        }
    }
    if (!out.nnue_info_path.has_value()) {
        if (const char* env_nnue_info_path = std::getenv("KOI_CLI_NNUE_INFO_PATH");
            env_nnue_info_path != nullptr && *env_nnue_info_path != '\0') {
            out.nnue_info_path = env_nnue_info_path;
        }
    }
    if (!out.book_path.has_value()) {
        if (const char* env_book_path = std::getenv("KOI_CLI_BOOK_PATH");
            env_book_path != nullptr && *env_book_path != '\0') {
            out.book_path = env_book_path;
        }
    }
    if (!out.syzygy_path.has_value()) {
        if (const char* env_syzygy_path = std::getenv("KOI_CLI_SYZYGY_PATH");
            env_syzygy_path != nullptr && *env_syzygy_path != '\0') {
            out.syzygy_path = env_syzygy_path;
        }
    }
    if (!out.see_move.has_value()) {
        if (const char* env_see = std::getenv("KOI_CLI_SEE_MOVE");
            env_see != nullptr && *env_see != '\0') {
            out.see_move = env_see;
        }
    }
    if (!out.inspect_fen.has_value()) {
        if (const char* env_fen = std::getenv("KOI_CLI_FEN");
            env_fen != nullptr && *env_fen != '\0') {
            out.inspect_fen = env_fen;
        }
    }
    if (!out.eval) {
        if (const char* env_eval = std::getenv("KOI_CLI_EVAL");
            env_eval != nullptr && std::string_view(env_eval) == "1") {
            out.eval = true;
        }
    }
    if (!out.options) {
        if (const char* env_options = std::getenv("KOI_CLI_OPTIONS");
            env_options != nullptr && std::string_view(env_options) == "1") {
            out.options = true;
        }
    }
    if (!out.eval_fen.has_value()) {
        if (const char* env_eval_fen = std::getenv("KOI_CLI_EVAL_FEN");
            env_eval_fen != nullptr && *env_eval_fen != '\0') {
            out.eval_fen = env_eval_fen;
        }
    }
    if (!out.perft_depth.has_value()) {
        if (const char* env_perft = std::getenv("KOI_CLI_PERFT");
            env_perft != nullptr && *env_perft != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_perft, 1, 10, parsed)) {
                out.perft_depth = static_cast<int>(parsed);
            }
        }
    }
    if (!out.perft_fen.has_value()) {
        if (const char* env_perft_fen = std::getenv("KOI_CLI_PERFT_FEN");
            env_perft_fen != nullptr && *env_perft_fen != '\0') {
            out.perft_fen = env_perft_fen;
        }
    }
    if (!out.bench_depth.has_value()) {
        if (const char* env_bench = std::getenv("KOI_BENCH_DEPTH");
            env_bench != nullptr && *env_bench != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_bench, 1, 64, parsed)) {
                out.bench_depth = static_cast<int>(parsed);
            }
        }
        if (const char* env_hash = std::getenv("KOI_BENCH_HASH");
            env_hash != nullptr && *env_hash != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_hash, 1, kMaximumBenchHashMb, parsed)) {
                out.bench_hash_mb = parsed;
            }
        }
        if (const char* env_threads = std::getenv("KOI_BENCH_THREADS");
            env_threads != nullptr && *env_threads != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_threads, 1, max_bench_threads, parsed)) {
                out.bench_threads = parsed;
            }
        }
    }
    if (!out.speedtest) {
        if (const char* env_speedtest = std::getenv("KOI_SPEEDTEST");
            env_speedtest != nullptr && std::string_view(env_speedtest) == "1") {
            out.speedtest = true;
        }
        if (const char* env_threads = std::getenv("KOI_SPEEDTEST_THREADS");
            env_threads != nullptr && *env_threads != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_threads, 1, max_bench_threads, parsed)) {
                out.speedtest_threads = parsed;
            }
        }
        if (const char* env_hash = std::getenv("KOI_SPEEDTEST_HASH");
            env_hash != nullptr && *env_hash != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_hash, 1, kMaximumBenchHashMb, parsed)) {
                out.speedtest_hash_mb = parsed;
            }
        }
        if (const char* env_seconds = std::getenv("KOI_SPEEDTEST_SECONDS");
            env_seconds != nullptr && *env_seconds != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_seconds, 1, 60, parsed)) {
                out.speedtest_seconds = static_cast<int>(parsed);
            }
        }
    }
    if (!out.commands.empty() &&
        (out.bench_depth.has_value() || out.speedtest || out.compiler || out.nnue_info ||
         out.display || out.fen || out.key || out.moves || out.flip || out.book || out.tb ||
         out.see_move.has_value() || out.eval || out.options || out.perft_depth.has_value())) {
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
    if (options.display || options.fen || options.key || options.moves || options.flip) {
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
        } else if (options.key) {
            std::cout << detail::format_key_info(*root) << std::flush;
        } else {
            std::cout << detail::format_legal_moves(*root) << std::flush;
        }
        return 0;
    }
    if (options.see_move.has_value()) {
        const std::optional<GameState> root = resolve_cli_position(options.inspect_fen);
        if (!root.has_value()) {
            return 2;
        }
        const std::optional<Move> move = Move::parse_uci(*options.see_move);
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
            book_path = OpeningBook::is_usable_file(primary) ? primary : packaged;
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
    if (options.perft_depth.has_value()) {
        const std::optional<GameState> resolved = resolve_cli_position(options.perft_fen);
        if (!resolved.has_value()) {
            return 2;
        }
        const GameState& root = *resolved;
        const std::vector<Move> legal_moves = root.legal_moves();
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t total = 0;
        std::vector<std::pair<Move, std::uint64_t>> counts;
        counts.reserve(legal_moves.size());
        for (const Move& move : legal_moves) {
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
