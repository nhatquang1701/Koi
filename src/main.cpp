#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

#if !defined(_WIN32)
#include <csignal>
#endif

#include "cli_support.hpp"
#include "koi/classical_evaluator.hpp"
#include "koi/cpu_features.hpp"
#include "koi/cpu_variant.hpp"
#include "koi/nnue.hpp"
#include "koi/search_service.hpp"
#include "koi/uci_controller.hpp"

namespace {

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
    // for the entry executable directly while CLI flags and subcommands
    // propagate via KOI_* environment variables.
    koi::cli::CliOptions cli_options;
    if (const std::optional<int> early_exit = koi::cli::parse_cli_options(
            argc, argv, KOI_ENGINE_BUILD_VERSION, KOI_ENGINE_BUILD_COMMIT,
            koi::UciController::kDefaultBenchDepth, koi::UciController::kDefaultBenchHashMb,
            koi::UciController::kDefaultBenchThreads, &koi::UciController::is_valid_protocol,
            cli_options);
        early_exit.has_value()) {
        return *early_exit;
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
    if (const std::optional<int> zero_net_exit = koi::cli::run_zero_network_cli_command(
            cli_options, argc, argv, KOI_ENGINE_BUILD_VERSION, KOI_ENGINE_BUILD_COMMIT);
        zero_net_exit.has_value()) {
        return *zero_net_exit;
    }
    // Bootstrap with a tiny table so a GUI/tournament can deliver its Hash
    // option before a large default allocation is charged to the first clock.
    // UciController materializes the configured 512 MB default at isready.
    //
    // Default NNUE evaluator selection: discover `koi.nnue` beside the binary
    // (or `KOI_NNUE_PATH`), unless `KOI_EVALUATOR=hce` is explicitly selected
    // below. `setoption name EvalFile` can load a different network at
    // runtime; a rejected file keeps the boot evaluator instead of failing.
    const std::filesystem::path engine_directory = koi::cli::executable_directory(argc, argv);
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
    if (koi::cli::hce_requested_from_environment()) {
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
    std::optional<std::istringstream> batch_input;
    if (!cli_options.commands.empty()) {
        batch_input.emplace(koi::cli::build_command_batch_script(cli_options.commands));
    }
    koi::UciController controller(
        batch_input.has_value() ? static_cast<std::istream&>(*batch_input) : std::cin,
        std::cout, std::cerr, std::move(search_service), engine_directory,
        autodiscovered_network && selection.nnue_enabled);
    if (cli_options.bench_depth.has_value()) {
        return controller.run_bench(*cli_options.bench_depth, cli_options.bench_hash_mb,
                                    cli_options.bench_threads);
    }
    if (cli_options.speedtest) {
        return controller.run_speedtest(cli_options.speedtest_threads,
                                        cli_options.speedtest_hash_mb,
                                        cli_options.speedtest_seconds);
    }
    if (cli_options.eval) {
        return controller.run_eval(cli_options.eval_fen
                                       ? std::optional<std::string_view>(*cli_options.eval_fen)
                                       : std::nullopt);
    }
    if (cli_options.initial_protocol.has_value()) {
        (void)controller.set_protocol(*cli_options.initial_protocol);
    }
    return controller.run();
}
