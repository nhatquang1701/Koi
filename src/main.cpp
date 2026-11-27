#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#if !defined(_WIN32)
#include <csignal>
#endif

#include "koi/classical_evaluator.hpp"
#include "koi/cpu_features.hpp"
#include "koi/cpu_variant.hpp"
#include "koi/detail/uci_support.hpp"
#include "koi/executable_path.hpp"
#include "koi/game_state.hpp"
#include "koi/nnue.hpp"
#include "koi/perft.hpp"
#include "koi/search_service.hpp"
#include "koi/uci_controller.hpp"

namespace {

// Development/measurement override: select the clean HCE without requiring a
// network at boot.  Release/default behavior is unchanged (NNUE required).
[[nodiscard]] bool hce_requested_from_environment() noexcept {
    const char* value = std::getenv("KOI_EVALUATOR");
    if (value == nullptr) {
        return false;
    }
    std::string_view requested(value);
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

#if defined(__linux__) && defined(__GLIBC__)
// The Windows build links with a 16 MiB stack reserve that worker threads
// inherit.  glibc derives new thread stacks from RLIMIT_STACK (8 MiB by
// default) and ignores the linker's PT_GNU_STACK size, so raise the process
// default before any search thread exists; deep tactical lines rely on it.
//
// macOS is skipped: Darwin has no process-wide default setter (Apple's pthread.h
// declares no pthread_setattr_default_np), so bare `std::thread` workers keep
// the 512 KiB default while `koi::detail::WorkerThread` raises its explicit
// stack size to 64 MiB and the main thread follows the shell's `ulimit -s`.
void configure_worker_stack() noexcept {
    pthread_attr_t attributes;
    if (pthread_getattr_default_np(&attributes) != 0) {
        return;
    }
    if (pthread_attr_setstacksize(&attributes, 16U * 1024U * 1024U) == 0) {
        (void)pthread_setattr_default_np(&attributes);
    }
    pthread_attr_destroy(&attributes);
}
#endif

std::filesystem::path resolved_executable_path(int argc, char* argv[]) {
    const std::filesystem::path module_path = koi::current_executable_path();
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

void write_engine_json(int argc, char* argv[]) {
    const std::string exe_path = resolved_executable_path(argc, argv).string();
    std::cout << "{\n"
              << "  \"type\": \"local\",\n"
              << "  \"name\": \"Koi\",\n"
              << "  \"version\": \"" << json_escape(KOI_ENGINE_BUILD_VERSION) << "\",\n"
              << "  \"path\": \"" << json_escape(exe_path) << "\",\n"
              << "  \"loaded\": true,\n"
              << "  \"settings\": [\n"
              << "    { \"name\": \"Threads\", \"value\": 1 },\n"
              << "    { \"name\": \"Hash\", \"value\": 512 }\n"
              << "  ]\n"
              << "}\n";
}

void write_help() {
    std::cout << "Koi " << KOI_ENGINE_BUILD_VERSION << " (" << KOI_ENGINE_BUILD_COMMIT
              << ") - C++26 UCI chess engine\n\n"
              << "Usage: koi-engine [options] [command]\n\n"
              << "Options & commands:\n"
              << "  -v, --version                            Print engine version and git commit\n"
              << "  -h, --help                               Show this help message\n"
              << "  --engine-json                            Print En Croissant / Pawn Appetit JSON profile\n"
              << "  --protocol <name>                        Start in protocol mode (uci, usi, ucci, ucicyclone, xboard)\n"
              << "  bench, --bench [depth [hash [threads]]]  Run OpenBench-compatible benchmark (defaults: 10 16 1)\n"
              << "  compiler, --compiler                     Print compiler, C++26 standard, and dispatched ISA\n"
              << "  eval, --eval [fen]                       Evaluate startpos or FEN with the active evaluator\n"
              << "  perft <depth> [fen]                      Run timed perft divide (depth 1..10)\n\n"
              << "Interactive UCI commands also include: d, fen, key, moves, eval, flip, see <move>, compiler, bench [depth [hash [threads]]], perft <depth>, wait, help\n";
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

int main(int argc, char* argv[]) {
#if !defined(_WIN32)
    // A GUI crash closes our stdout. Without this, the first protocol write
    // raises SIGPIPE and terminates the engine; ignoring it lets the stream
    // fail silently while the controller keeps serving until EOF/quit.
    std::signal(SIGPIPE, SIG_IGN);
#endif
#if defined(__linux__) && defined(__GLIBC__)
    configure_worker_stack();
#endif
#if defined(NDEBUG) && defined(KOI_CPU_REQUIRES_AVX512)
    if (!koi::cpu_supports_avx512()) {
        std::cerr << "Koi Release requires an x64 CPU with AVX-512 support.\n";
        return 3;
    }
#endif
#if defined(NDEBUG) && defined(KOI_CPU_REQUIRES_AVX2)
    if (!koi::cpu_supports_avx2()) {
        std::cerr << "Koi Release requires an x64 CPU with AVX2 support.\n";
        return 3;
    }
#endif
    // Parse CLI options before `run_cpu_selector()`: selector re-exec strips
    // argv, so metadata queries (`--version`, `--help`, `--engine-json`) answer
    // for the entry executable directly while `--protocol` and `bench` propagate
    // via KOI_PROTOCOL / KOI_BENCH_DEPTH.
    std::optional<std::string_view> initial_protocol;
    std::optional<int> bench_depth;
    std::size_t bench_hash_mb = koi::UciController::kDefaultBenchHashMb;
    std::size_t bench_threads = koi::UciController::kDefaultBenchThreads;
    bool cli_compiler = false;
    bool cli_eval = false;
    std::optional<std::string> cli_eval_fen;
    std::optional<int> cli_perft_depth;
    std::optional<std::string> cli_perft_fen;
    constexpr std::size_t kMaximumBenchHashMb = 4'096;
    const std::size_t max_bench_threads = koi::maximum_search_threads();
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
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--version" || arg == "-v") {
            std::cout << "Koi " << KOI_ENGINE_BUILD_VERSION << " (" << KOI_ENGINE_BUILD_COMMIT
                      << ")\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            write_help();
            return 0;
        }
        if (arg == "--engine-json") {
            write_engine_json(argc, argv);
            return 0;
        }
        if (arg == "compiler" || arg == "--compiler") {
            cli_compiler = true;
            set_environment_value("KOI_CLI_COMPILER", "1");
            continue;
        }
        if (arg == "eval" || arg == "--eval") {
            cli_eval = true;
            set_environment_value("KOI_CLI_EVAL", "1");
            collect_trailing_fen(index, cli_eval_fen);
            if (cli_eval_fen.has_value()) {
                set_environment_value("KOI_CLI_EVAL_FEN", *cli_eval_fen);
            }
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
            cli_perft_depth = static_cast<int>(depth);
            set_environment_value("KOI_CLI_PERFT", std::to_string(depth));
            collect_trailing_fen(index, cli_perft_fen);
            if (cli_perft_fen.has_value()) {
                set_environment_value("KOI_CLI_PERFT_FEN", *cli_perft_fen);
            }
            continue;
        }
        if (arg == "bench" || arg == "--bench") {
            std::size_t depth = static_cast<std::size_t>(koi::UciController::kDefaultBenchDepth);
            std::size_t hash_mb = koi::UciController::kDefaultBenchHashMb;
            std::size_t threads = koi::UciController::kDefaultBenchThreads;
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
            bench_depth = static_cast<int>(depth);
            bench_hash_mb = hash_mb;
            bench_threads = threads;
            set_environment_value("KOI_BENCH_DEPTH", std::to_string(depth));
            set_environment_value("KOI_BENCH_HASH", std::to_string(hash_mb));
            set_environment_value("KOI_BENCH_THREADS", std::to_string(threads));
            continue;
        }
        if (arg == "--protocol") {
            if (index + 1 >= argc) {
                std::cerr << "koi-engine: missing argument for --protocol (expected uci, usi, "
                             "ucci, ucicyclone, or xboard).\n";
                return 2;
            }
            initial_protocol = std::string_view(argv[++index]);
        } else if (arg.starts_with("--protocol=")) {
            initial_protocol = arg.substr(std::string_view("--protocol=").size());
        } else {
            std::cerr << "koi-engine: unknown option '" << arg
                      << "' (supported: --version, --help, --engine-json, --protocol <name>, "
                         "bench [depth [hash [threads]]], compiler, eval [fen], perft <depth> [fen]).\n";
            return 2;
        }
        if (initial_protocol.has_value()) {
            if (!koi::UciController::is_valid_protocol(*initial_protocol)) {
                std::cerr << "koi-engine: unknown protocol '" << *initial_protocol
                          << "' (supported: uci, usi, ucci, ucicyclone, xboard).\n";
                return 2;
            }
            set_environment_value("KOI_PROTOCOL", *initial_protocol);
        }
    }
    if (!initial_protocol.has_value()) {
        if (const char* env_protocol = std::getenv("KOI_PROTOCOL");
            env_protocol != nullptr && *env_protocol != '\0' &&
            koi::UciController::is_valid_protocol(env_protocol)) {
            initial_protocol = std::string_view(env_protocol);
        }
    }
    if (!cli_compiler) {
        if (const char* env_compiler = std::getenv("KOI_CLI_COMPILER");
            env_compiler != nullptr && std::string_view(env_compiler) == "1") {
            cli_compiler = true;
        }
    }
    if (!cli_eval) {
        if (const char* env_eval = std::getenv("KOI_CLI_EVAL");
            env_eval != nullptr && std::string_view(env_eval) == "1") {
            cli_eval = true;
        }
    }
    if (!cli_eval_fen.has_value()) {
        if (const char* env_eval_fen = std::getenv("KOI_CLI_EVAL_FEN");
            env_eval_fen != nullptr && *env_eval_fen != '\0') {
            cli_eval_fen = env_eval_fen;
        }
    }
    if (!cli_perft_depth.has_value()) {
        if (const char* env_perft = std::getenv("KOI_CLI_PERFT");
            env_perft != nullptr && *env_perft != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_perft, 1, 10, parsed)) {
                cli_perft_depth = static_cast<int>(parsed);
            }
        }
    }
    if (!cli_perft_fen.has_value()) {
        if (const char* env_perft_fen = std::getenv("KOI_CLI_PERFT_FEN");
            env_perft_fen != nullptr && *env_perft_fen != '\0') {
            cli_perft_fen = env_perft_fen;
        }
    }
    if (!bench_depth.has_value()) {
        if (const char* env_bench = std::getenv("KOI_BENCH_DEPTH");
            env_bench != nullptr && *env_bench != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_bench, 1, 64, parsed)) {
                bench_depth = static_cast<int>(parsed);
            }
        }
        if (const char* env_hash = std::getenv("KOI_BENCH_HASH");
            env_hash != nullptr && *env_hash != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_hash, 1, kMaximumBenchHashMb, parsed)) {
                bench_hash_mb = parsed;
            }
        }
        if (const char* env_threads = std::getenv("KOI_BENCH_THREADS");
            env_threads != nullptr && *env_threads != '\0') {
            std::size_t parsed = 0;
            if (parse_bounded_uint(env_threads, 1, max_bench_threads, parsed)) {
                bench_threads = parsed;
            }
        }
    }
#if defined(KOI_CPU_SELECTOR)
    // The baseline build runs on any x64 CPU. When a faster sibling executable
    // sits next to this binary and the host supports it, re-exec through that
    // binary so the AVX2 or AVX-512 code paths are used without asking the user
    // to pick a flavor. A missing or unstartable sibling keeps this process.
    // run_cpu_selector() returns -1 when this process must continue; any other
    // value is a child exit code. A child that crashed on Windows returns a
    // negative int (0xC0000005 in DWORD form), which must still stop this
    // process instead of booting a second engine session.
    const int selector_exit = koi::run_cpu_selector();
    if (selector_exit != -1) {
        return selector_exit;
    }
#endif
    if (cli_compiler) {
        std::cout << koi::detail::format_compiler_info(
            KOI_ENGINE_BUILD_VERSION, KOI_ENGINE_BUILD_COMMIT) << std::flush;
        return 0;
    }
    if (cli_perft_depth.has_value()) {
        koi::GameState root = koi::GameState::startpos();
        if (cli_perft_fen.has_value()) {
            const auto parsed = koi::GameState::from_fen(*cli_perft_fen);
            if (!parsed.has_value()) {
                std::cerr << "koi-engine: invalid FEN '" << *cli_perft_fen << "'\n";
                return 2;
            }
            root = *parsed;
        }
        const std::vector<koi::Move> legal_moves = root.legal_moves();
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t total = 0;
        for (const koi::Move& move : legal_moves) {
            koi::GameState child = root;
            if (!child.make_move(move)) {
                continue;
            }
            const std::uint64_t nodes = koi::perft(child, *cli_perft_depth - 1);
            total += nodes;
            std::cout << move.uci() << ": " << nodes << '\n';
        }
        const auto elapsed_ms = std::max<std::int64_t>(
            1, std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
                   .count());
        const std::uint64_t nps = (total * 1000ULL) / static_cast<std::uint64_t>(elapsed_ms);
        std::cout << "\nNodes searched: " << total << '\n'
                  << "Time (ms)     : " << elapsed_ms << '\n'
                  << "Nodes/second  : " << nps << '\n'
                  << std::flush;
        return 0;
    }
    // Bootstrap with a tiny table so a GUI/tournament can deliver its Hash
    // option before a large default allocation is charged to the first clock.
    // UciController materializes the configured 512 MB default at isready.
    //
    // Default NNUE evaluator selection: discover `koi.nnue` beside the binary
    // (or `KOI_NNUE_PATH`), unless `KOI_EVALUATOR=hce` is explicitly selected
    // below. `setoption name EvalFile` can load a different network at
    // runtime; a rejected file keeps the boot evaluator instead of failing.
    const std::filesystem::path engine_directory = executable_directory(argc, argv);
    std::filesystem::path nnue_path;
    bool autodiscovered_network = false;
    if (const char* environment_path = std::getenv("KOI_NNUE_PATH");
        environment_path != nullptr && *environment_path != '\0') {
        nnue_path = environment_path;
    } else {
        // Auto-discovery: `koi.nnue` beside the binary, packaged `nnue/koi.nnue`,
        // or a single `*.nnue` in either place.  Falls back to the working
        // directory for development runs.  KOI_NNUE_AUTODETECT=0 disables it and
        // `EvalFile` still overrides the boot-time choice at runtime.
        if (auto discovered = koi::discover_nnue_network(engine_directory);
            discovered.has_value()) {
            nnue_path = *discovered;
            autodiscovered_network = true;
        } else {
            std::error_code working_error;
            const std::filesystem::path working_directory =
                std::filesystem::current_path(working_error);
            if (!working_error && working_directory != engine_directory) {
                if (auto discovered = koi::discover_nnue_network(working_directory);
                    discovered.has_value()) {
                    nnue_path = *discovered;
                    autodiscovered_network = true;
                }
            }
        }
    }
    koi::EvaluatorSelection selection;
    if (hce_requested_from_environment()) {
        // Explicit development/measurement override: boot without a network.
        selection.evaluator = std::make_shared<koi::ClassicalEvaluator>();
    } else {
        try {
            selection = nnue_path.empty()
                ? koi::make_evaluator()
                : koi::make_evaluator(std::optional<std::filesystem::path>(nnue_path));
        } catch (const std::exception& error) {
            std::cerr << "koi-engine: unable to load an evaluator network (" << error.what()
                      << ").\n"
                      << "koi-engine: provide koi.nnue beside the executable or set EvalFile.\n";
            return 4;
        }
    }
    if (selection.evaluator == nullptr) {
        std::cerr << "koi-engine: no usable NNUE network";
        if (selection.nnue_error.has_value()) {
            std::cerr << " (" << selection.nnue_error->message << ")";
        }
        std::cerr << ".\n"
                  << "koi-engine: provide koi.nnue beside the executable or set EvalFile.\n";
        return 4;
    }
    koi::SearchService search_service(selection.evaluator, {}, 1);
    koi::UciController controller(std::cin, std::cout, std::cerr, std::move(search_service),
                                  engine_directory,
                                  autodiscovered_network && selection.nnue_enabled);
    if (bench_depth.has_value()) {
        return controller.run_bench(*bench_depth, bench_hash_mb, bench_threads);
    }
    if (cli_eval) {
        return controller.run_eval(cli_eval_fen ? std::optional<std::string_view>(*cli_eval_fen)
                                                : std::nullopt);
    }
    if (initial_protocol.has_value()) {
        (void)controller.set_protocol(*initial_protocol);
    }
    return controller.run();
}
