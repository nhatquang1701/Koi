#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli_support.hpp"
#include "koi_test_support.hpp"

namespace {

constexpr std::array<const char*, 33> kCliEnvVars = {
    "KOI_PROTOCOL",
    "KOI_CLI_HASH",
    "KOI_CLI_THREADS",
    "KOI_EVALUATOR",
    "KOI_NNUE_PATH",
    "KOI_NNUE_AUTODETECT",
    "KOI_CLI_COMMANDS",
    "KOI_CLI_COMPILER",
    "KOI_CLI_DISPLAY",
    "KOI_CLI_FEN_CMD",
    "KOI_CLI_KEY",
    "KOI_CLI_MOVES",
    "KOI_CLI_FLIP",
    "KOI_CLI_BOOK",
    "KOI_CLI_TB",
    "KOI_CLI_NNUE_INFO",
    "KOI_CLI_NNUE_INFO_PATH",
    "KOI_CLI_BOOK_PATH",
    "KOI_CLI_SYZYGY_PATH",
    "KOI_CLI_SEE_MOVE",
    "KOI_CLI_FEN",
    "KOI_CLI_EVAL",
    "KOI_CLI_OPTIONS",
    "KOI_CLI_EVAL_FEN",
    "KOI_CLI_PERFT",
    "KOI_CLI_PERFT_FEN",
    "KOI_BENCH_DEPTH",
    "KOI_BENCH_HASH",
    "KOI_BENCH_THREADS",
    "KOI_SPEEDTEST",
    "KOI_SPEEDTEST_THREADS",
    "KOI_SPEEDTEST_HASH",
    "KOI_SPEEDTEST_SECONDS",
};

void set_or_unset_env(const char* name, const std::optional<std::string>& value) noexcept {
#if defined(_WIN32)
    if (value.has_value()) {
        (void)::_putenv_s(name, value->c_str());
    } else {
        (void)::_putenv_s(name, "");
    }
#else
    if (value.has_value()) {
        (void)::setenv(name, value->c_str(), 1);
    } else {
        (void)::unsetenv(name);
    }
#endif
}

class ScopedCliEnvGuard final {
public:
    ScopedCliEnvGuard() {
        for (std::size_t idx = 0; idx < kCliEnvVars.size(); ++idx) {
            if (const char* current = std::getenv(kCliEnvVars[idx]); current != nullptr) {
                saved_[idx] = std::string(current);
            }
        }
        clear_all();
    }

    ~ScopedCliEnvGuard() {
        for (std::size_t idx = 0; idx < kCliEnvVars.size(); ++idx) {
            set_or_unset_env(kCliEnvVars[idx], saved_[idx]);
        }
    }

    ScopedCliEnvGuard(const ScopedCliEnvGuard&) = delete;
    ScopedCliEnvGuard& operator=(const ScopedCliEnvGuard&) = delete;

    void clear_all() const noexcept {
        for (const char* name : kCliEnvVars) {
            set_or_unset_env(name, std::nullopt);
        }
    }

private:
    std::array<std::optional<std::string>, kCliEnvVars.size()> saved_;
};

class ScopedStreamCapture final {
public:
    explicit ScopedStreamCapture(std::ostream& stream)
        : stream_(stream), previous_(stream.rdbuf(buffer_.rdbuf())) {}

    ~ScopedStreamCapture() {
        stream_.rdbuf(previous_);
    }

    ScopedStreamCapture(const ScopedStreamCapture&) = delete;
    ScopedStreamCapture& operator=(const ScopedStreamCapture&) = delete;

    [[nodiscard]] std::string str() const {
        return buffer_.str();
    }

private:
    std::ostream& stream_;
    std::ostringstream buffer_;
    std::streambuf* previous_ = nullptr;
};

bool is_valid_protocol_stub(const std::string_view name) noexcept {
    return name == "uci" || name == "usi" || name == "ucci" || name == "ucicyclone" ||
           name == "xboard";
}

std::optional<int> invoke_parse(const std::vector<std::string>& args, koi::cli::CliOptions& out) {
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.emplace_back("koi-engine");
    storage.insert(storage.end(), args.begin(), args.end());

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& arg : storage) {
        argv.push_back(arg.data());
    }
    return koi::cli::parse_cli_options(
        static_cast<int>(argv.size()), argv.data(), "1.2.0", "testsha", 10, 16, 1,
        &is_valid_protocol_stub, out);
}

void test_batch_script_builder() {
    const std::string auto_terminated = koi::cli::build_command_batch_script(
        {"uci", "isready", "position startpos", "go depth 2"});
    koi::test::require(auto_terminated == "uci\nisready\nposition startpos\ngo depth 2\nwait\nquit\n",
                       "normal batch commands must auto-append wait and quit");

    const std::string explicit_quit =
        koi::cli::build_command_batch_script({"uci", "  quit  \r\n\n"});
    koi::test::require(explicit_quit == "uci\n  quit  \n",
                       "trailing quit must suppress duplicate wait/quit even with whitespace or CRLF");

    const std::string explicit_bye =
        koi::cli::build_command_batch_script({"ucci\r\n", "bye   "});
    koi::test::require(explicit_bye == "ucci\nbye   \n",
                       "trailing bye must suppress automatic wait/quit");

    const std::string multiline =
        koi::cli::build_command_batch_script({"position startpos\r\ngo depth 1\r\n", "   "});
    koi::test::require(multiline == "position startpos\ngo depth 1\nwait\nquit\n",
                       "whitespace-only trailing entries must not hide the last command");
}

void test_inline_cmd_parsing_and_env_roundtrip() {
    ScopedCliEnvGuard env;
    koi::cli::CliOptions options;

    const auto parsed =
        invoke_parse({"-c", "uci", "--cmd", "isready", "--cmd=go depth 1"}, options);
    koi::test::require(!parsed.has_value(), "valid -c/--cmd flags must continue engine startup");
    koi::test::require(options.commands.size() == 3 && options.commands[0] == "uci" &&
                           options.commands[1] == "isready" && options.commands[2] == "go depth 1",
                       "repeatable -c/--cmd/--cmd= must preserve command order");

    koi::cli::CliOptions reexec_options;
    const auto reexec = invoke_parse({}, reexec_options);
    koi::test::require(!reexec.has_value() && reexec_options.commands == options.commands,
                       "empty argv must restore KOI_CLI_COMMANDS after CPU selector re-exec");

    {
        env.clear_all();
        ScopedStreamCapture out(std::cout);
        koi::cli::CliOptions ver_opts;
        const auto exit_code = invoke_parse({"version"}, ver_opts);
        koi::test::require(exit_code == std::optional<int>(0) &&
                               out.str().find("Koi ") == 0,
                           "bare version subcommand must exit 0 with version banner");
    }
    {
        env.clear_all();
        ScopedStreamCapture out(std::cout);
        koi::cli::CliOptions help_opts;
        const auto exit_code = invoke_parse({"help"}, help_opts);
        koi::test::require(exit_code == std::optional<int>(0) &&
                               out.str().find("Usage: koi-engine") != std::string::npos &&
                               out.str().find("history") != std::string::npos,
                           "bare help subcommand must exit 0 with usage summary");
    }

    for (const std::vector<std::string>& invalid : std::vector<std::vector<std::string>>{
             {"-c"},
             {"--cmd="},
             {"--cmd", ""},
         }) {
        env.clear_all();
        ScopedStreamCapture err(std::cerr);
        koi::cli::CliOptions bad_options;
        const auto exit_code = invoke_parse(invalid, bad_options);
        koi::test::require(exit_code == std::optional<int>(2),
                           "missing or empty -c/--cmd argument must exit 2");
        koi::test::require(err.str().find("missing command for --cmd") != std::string::npos,
                           "missing or empty --cmd error must explain why it failed");
    }

    for (const std::string_view subcommand :
         {"bench", "speedtest", "nnue", "eval", "options", "compiler", "fen"}) {
        env.clear_all();
        ScopedStreamCapture err(std::cerr);
        koi::cli::CliOptions conflict_options;
        const auto exit_code = invoke_parse({"-c", "uci", std::string(subcommand)}, conflict_options);
        koi::test::require(exit_code == std::optional<int>(2),
                           "--cmd combined with a one-shot subcommand must exit 2");
        koi::test::require(err.str().find("cannot be combined") != std::string::npos,
                           "conflicting --cmd and one-shot subcommand must emit diagnostic");
    }
}

void test_evaluator_and_asset_flags() {
    ScopedCliEnvGuard env;

    koi::cli::CliOptions hce_options;
    koi::test::require(!invoke_parse({"--hce", "eval"}, hce_options).has_value() &&
                           hce_options.eval && koi::cli::hce_requested_from_environment(),
                       "--hce must enable HCE in the environment");

    koi::cli::CliOptions nnue_options;
    koi::test::require(!invoke_parse({"--nnue=/tmp/test.nnue", "eval"}, nnue_options).has_value() &&
                           nnue_options.eval && !koi::cli::hce_requested_from_environment(),
                       "--nnue= must override KOI_EVALUATOR=hce");
    const char* env_nnue = std::getenv("KOI_NNUE_PATH");
    koi::test::require(env_nnue != nullptr && std::string_view(env_nnue) == "/tmp/test.nnue",
                       "--nnue= must export KOI_NNUE_PATH");

    {
        env.clear_all();
        ScopedStreamCapture err(std::cerr);
        koi::cli::CliOptions conflict;
        const auto exit_code = invoke_parse({"--hce", "--nnue", "/tmp/a.nnue"}, conflict);
        koi::test::require(exit_code == std::optional<int>(2) &&
                               err.str().find("cannot be combined") != std::string::npos,
                           "--hce and --nnue together must exit 2");
    }

    env.clear_all();
    koi::cli::CliOptions asset_options;
    koi::test::require(
        !invoke_parse({"--book", "/tmp/book.bin", "--syzygy=/tmp/tb", "tb"}, asset_options).has_value(),
        "--book and --syzygy= must parse cleanly");
    koi::test::require(asset_options.tb && asset_options.book_path == "/tmp/book.bin" &&
                           asset_options.syzygy_path == "/tmp/tb",
                       "asset options must store supplied paths");
    koi::cli::CliOptions asset_reexec;
    koi::test::require(!invoke_parse({}, asset_reexec).has_value() && asset_reexec.tb &&
                           asset_reexec.book_path == "/tmp/book.bin" &&
                           asset_reexec.syzygy_path == "/tmp/tb",
                       "asset options and tb flag must roundtrip across env");

    env.clear_all();
    koi::cli::CliOptions nnue_default;
    koi::test::require(!invoke_parse({"nnue"}, nnue_default).has_value() &&
                           nnue_default.nnue_info && !nnue_default.nnue_info_path.has_value(),
                       "nnue without path must enable nnue_info discovery");
    env.clear_all();
    koi::cli::CliOptions nnue_explicit;
    koi::test::require(!invoke_parse({"nnue", "/tmp/model.nnue"}, nnue_explicit).has_value() &&
                           nnue_explicit.nnue_info &&
                           nnue_explicit.nnue_info_path == "/tmp/model.nnue",
                       "nnue <path> must store explicit target path");
    koi::cli::CliOptions nnue_reexec;
    koi::test::require(!invoke_parse({}, nnue_reexec).has_value() && nnue_reexec.nnue_info &&
                           nnue_reexec.nnue_info_path == "/tmp/model.nnue",
                       "KOI_CLI_NNUE_INFO* must roundtrip across empty argv");

    for (const std::string_view missing_flag : {"--nnue", "--book", "--syzygy="}) {
        env.clear_all();
        ScopedStreamCapture err(std::cerr);
        koi::cli::CliOptions missing_opts;
        const auto exit_code = invoke_parse({std::string(missing_flag)}, missing_opts);
        koi::test::require(exit_code == std::optional<int>(2) &&
                               err.str().find("missing path") != std::string::npos,
                           "missing asset path must exit 2");
    }

    env.clear_all();
    koi::cli::CliOptions runtime_opts;
    koi::test::require(
        !invoke_parse({"--hash", "64", "--threads", "1", "options"}, runtime_opts).has_value() &&
            runtime_opts.initial_hash_mb == std::optional<std::size_t>(64) &&
            runtime_opts.initial_threads == std::optional<std::size_t>(1) &&
            runtime_opts.options,
        "--hash and --threads must parse and combine with options");
    koi::cli::CliOptions runtime_reexec;
    koi::test::require(!invoke_parse({}, runtime_reexec).has_value() &&
                           runtime_reexec.initial_hash_mb == std::optional<std::size_t>(64) &&
                           runtime_reexec.initial_threads == std::optional<std::size_t>(1),
                       "KOI_CLI_HASH and KOI_CLI_THREADS must roundtrip across empty argv");

    env.clear_all();
    koi::cli::CliOptions inline_runtime;
    koi::test::require(
        !invoke_parse({"--hash=128", "--threads=1", "-c", "go depth 1"}, inline_runtime)
             .has_value() &&
            inline_runtime.initial_hash_mb == std::optional<std::size_t>(128) &&
            inline_runtime.initial_threads == std::optional<std::size_t>(1) &&
            inline_runtime.commands.size() == 1,
        "--hash= and --threads= inline forms must parse cleanly and combine with -c");

    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"--hash"},
             {"--hash="},
             {"--hash=0"},
             {"--hash=99999"},
             {"--threads"},
             {"--threads="},
             {"--threads=0"},
             {"--threads=abc"},
             {"--engine-json", "--hash", "0"},
         }) {
        env.clear_all();
        ScopedStreamCapture err(std::cerr);
        koi::cli::CliOptions invalid_runtime;
        const auto exit_code = invoke_parse(bad, invalid_runtime);
        koi::test::require(exit_code == std::optional<int>(2) && !err.str().empty(),
                           "missing or out-of-range --hash/--threads must exit 2");
    }

    {
        env.clear_all();
        ScopedStreamCapture out(std::cout);
        koi::cli::CliOptions json_opts;
        const auto exit_code = invoke_parse({"--engine-json"}, json_opts);
        const std::string printed = out.str();
        koi::test::require(exit_code == std::optional<int>(0) &&
                               printed.find("\"Threads\", \"value\": 1") != std::string::npos &&
                               printed.find("\"Hash\", \"value\": 512") != std::string::npos,
                           "bare --engine-json must default to Threads=1 and Hash=512");
    }
    {
        env.clear_all();
        ScopedStreamCapture out(std::cout);
        koi::cli::CliOptions json_opts;
        const auto exit_code =
            invoke_parse({"--threads", "1", "--hash", "256", "--engine-json"}, json_opts);
        const std::string printed = out.str();
        koi::test::require(exit_code == std::optional<int>(0) &&
                               printed.find("\"Threads\", \"value\": 1") != std::string::npos &&
                               printed.find("\"Hash\", \"value\": 256") != std::string::npos,
                           "--threads and --hash before --engine-json must override JSON settings");
    }
    {
        env.clear_all();
        ScopedStreamCapture out(std::cout);
        koi::cli::CliOptions json_opts;
        const auto exit_code =
            invoke_parse({"--engine-json", "--hash=128", "--threads=1"}, json_opts);
        const std::string printed = out.str();
        koi::test::require(exit_code == std::optional<int>(0) &&
                               printed.find("\"Threads\", \"value\": 1") != std::string::npos &&
                               printed.find("\"Hash\", \"value\": 128") != std::string::npos,
                           "--hash= and --threads= after --engine-json must override JSON settings");
    }
}

void test_unquoted_fen_and_inspection_subcommands() {
    ScopedCliEnvGuard env;

    koi::cli::CliOptions perft_opts;
    const auto perft_exit = invoke_parse(
        {"perft", "3", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", "w", "KQkq", "-", "0", "1"},
        perft_opts);
    koi::test::require(!perft_exit.has_value() && perft_opts.perft_depth == 3 &&
                           perft_opts.perft_fen ==
                               "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                       "perft must join unquoted 6-field FEN including standalone '-' en passant");

    koi::cli::CliOptions see_opts;
    const auto see_exit = invoke_parse(
        {"see", "e2e4", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", "w", "KQkq", "-", "0", "1"},
        see_opts);
    koi::test::require(!see_exit.has_value() && see_opts.see_move == "e2e4" &&
                           see_opts.inspect_fen ==
                               "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                       "see must preserve move and trailing unquoted FEN");

    for (const std::string_view cmd : {"d", "--display", "fen", "key", "moves", "flip", "book", "tb"}) {
        env.clear_all();
        koi::cli::CliOptions inspect_opts;
        const auto exit_code = invoke_parse({std::string(cmd), "8/8/8/8/8/8/4K3/4k3", "w", "-", "-", "0", "1"}, inspect_opts);
        koi::test::require(!exit_code.has_value() &&
                               inspect_opts.inspect_fen == "8/8/8/8/8/8/4K3/4k3 w - - 0 1",
                           "zero-network inspection subcommands must parse unquoted FENs");
    }

    {
        env.clear_all();
        koi::cli::CliOptions fen_opts;
        koi::test::require(!invoke_parse({"fen", "moves", "e2e4", "e7e5"}, fen_opts).has_value() &&
                               fen_opts.fen && fen_opts.inspect_fen == "moves e2e4 e7e5",
                           "fen subcommand must store trailing position spec");
        koi::cli::CliOptions fen_reexec;
        koi::test::require(!invoke_parse({}, fen_reexec).has_value() && fen_reexec.fen &&
                               fen_reexec.inspect_fen == "moves e2e4 e7e5",
                           "KOI_CLI_FEN_CMD and KOI_CLI_FEN must roundtrip across empty argv");
    }

    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"perft"},
             {"perft", "0"},
             {"perft", "11"},
             {"see"},
         }) {
        env.clear_all();
        ScopedStreamCapture err(std::cerr);
        koi::cli::CliOptions bad_opts;
        koi::test::require(invoke_parse(bad, bad_opts) == std::optional<int>(2) && !err.str().empty(),
                           "invalid perft or incomplete see command must exit 2 with diagnostic");
    }
}

void test_bench_and_protocol_bounds() {
    ScopedCliEnvGuard env;

    koi::cli::CliOptions bench_opts;
    koi::test::require(!invoke_parse({"bench", "6", "32", "2"}, bench_opts).has_value() &&
                           bench_opts.bench_depth == 6 && bench_opts.bench_hash_mb == 32 &&
                           bench_opts.bench_threads == 2,
                       "bench depth, hash, and threads must parse explicitly");

    koi::cli::CliOptions reexec_bench;
    koi::test::require(!invoke_parse({}, reexec_bench).has_value() &&
                           reexec_bench.bench_depth == 6 && reexec_bench.bench_hash_mb == 32 &&
                           reexec_bench.bench_threads == 2,
                       "KOI_BENCH_* variables must roundtrip across empty argv");

    env.clear_all();
    koi::cli::CliOptions speed_opts;
    koi::test::require(!invoke_parse({"speedtest", "1", "32", "5"}, speed_opts).has_value() &&
                           speed_opts.speedtest && speed_opts.speedtest_threads == 1 &&
                           speed_opts.speedtest_hash_mb == 32 && speed_opts.speedtest_seconds == 5,
                       "speedtest threads, hash, and seconds must parse explicitly");

    koi::cli::CliOptions reexec_speed;
    koi::test::require(!invoke_parse({}, reexec_speed).has_value() && reexec_speed.speedtest &&
                           reexec_speed.speedtest_threads == 1 &&
                           reexec_speed.speedtest_hash_mb == 32 &&
                           reexec_speed.speedtest_seconds == 5,
                       "KOI_SPEEDTEST* variables must roundtrip across empty argv");

    env.clear_all();
    koi::cli::CliOptions flag_bench_opts;
    koi::test::require(
        !invoke_parse({"--hash", "64", "--threads", "1", "bench", "4"}, flag_bench_opts)
             .has_value() &&
            flag_bench_opts.bench_depth == 4 && flag_bench_opts.bench_hash_mb == 64 &&
            flag_bench_opts.bench_threads == 1,
        "--hash and --threads must populate omitted bench positional arguments");
    koi::cli::CliOptions flag_bench_reexec;
    koi::test::require(!invoke_parse({}, flag_bench_reexec).has_value() &&
                           flag_bench_reexec.bench_depth == 4 &&
                           flag_bench_reexec.bench_hash_mb == 64 &&
                           flag_bench_reexec.bench_threads == 1,
                       "--hash/--threads fallback for bench must roundtrip across empty argv");

    env.clear_all();
    koi::cli::CliOptions pos_bench_opts;
    koi::test::require(
        !invoke_parse({"--hash", "64", "bench", "4", "32", "1"}, pos_bench_opts).has_value() &&
            pos_bench_opts.bench_depth == 4 && pos_bench_opts.bench_hash_mb == 32 &&
            pos_bench_opts.bench_threads == 1,
        "explicit positional bench hash must override --hash");
    koi::cli::CliOptions pos_bench_reexec;
    koi::test::require(!invoke_parse({}, pos_bench_reexec).has_value() &&
                           pos_bench_reexec.bench_hash_mb == 32,
                       "positional bench hash override must survive empty-argv re-exec");

    env.clear_all();
    koi::cli::CliOptions flag_speed_opts;
    koi::test::require(
        !invoke_parse({"--hash=32", "--threads=1", "speedtest"}, flag_speed_opts).has_value() &&
            flag_speed_opts.speedtest && flag_speed_opts.speedtest_hash_mb == 32 &&
            flag_speed_opts.speedtest_threads == 1,
        "--hash= and --threads= must populate omitted speedtest positional arguments");
    koi::cli::CliOptions flag_speed_reexec;
    koi::test::require(!invoke_parse({}, flag_speed_reexec).has_value() &&
                           flag_speed_reexec.speedtest &&
                           flag_speed_reexec.speedtest_hash_mb == 32 &&
                           flag_speed_reexec.speedtest_threads == 1,
                       "--hash/--threads fallback for speedtest must roundtrip across empty argv");

    env.clear_all();
    koi::cli::CliOptions pos_speed_opts;
    koi::test::require(
        !invoke_parse({"--hash=64", "speedtest", "1", "16", "2"}, pos_speed_opts).has_value() &&
            pos_speed_opts.speedtest && pos_speed_opts.speedtest_hash_mb == 16 &&
            pos_speed_opts.speedtest_threads == 1 && pos_speed_opts.speedtest_seconds == 2,
        "explicit positional speedtest hash must override --hash=");
    koi::cli::CliOptions pos_speed_reexec;
    koi::test::require(!invoke_parse({}, pos_speed_reexec).has_value() &&
                           pos_speed_reexec.speedtest_hash_mb == 16,
                       "positional speedtest hash override must survive empty-argv re-exec");

    env.clear_all();
    koi::cli::CliOptions opt_flags;
    koi::test::require(!invoke_parse({"--options"}, opt_flags).has_value() && opt_flags.options,
                       "--options must enable options readout");
    koi::cli::CliOptions reexec_opts;
    koi::test::require(!invoke_parse({}, reexec_opts).has_value() && reexec_opts.options,
                       "KOI_CLI_OPTIONS must roundtrip across empty argv");

    env.clear_all();
    koi::cli::CliOptions proto_opts;
    koi::test::require(!invoke_parse({"--protocol=xboard"}, proto_opts).has_value() &&
                           proto_opts.initial_protocol == "xboard",
                       "--protocol=xboard must set initial protocol");
    koi::cli::CliOptions proto_reexec;
    koi::test::require(!invoke_parse({}, proto_reexec).has_value() &&
                           proto_reexec.initial_protocol == "xboard",
                       "KOI_PROTOCOL must roundtrip across empty argv");

    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {"bench", "65"},
             {"bench", "4", "4097"},
             {"bench", "4", "16", "0"},
             {"speedtest", "0"},
             {"speedtest", "1", "4097"},
             {"speedtest", "1", "16", "61"},
             {"--protocol", "invalid"},
             {"--unknown-flag"},
         }) {
        env.clear_all();
        ScopedStreamCapture err(std::cerr);
        koi::cli::CliOptions bad_opts;
        koi::test::require(invoke_parse(bad, bad_opts) == std::optional<int>(2) && !err.str().empty(),
                           "out-of-range bench arguments, unknown protocols, and unknown flags must exit 2");
    }
}

void test_boot_nnue_path_resolution() {
    ScopedCliEnvGuard env;
    koi::test::TempDirectory scratch;
    const std::filesystem::path discovered_net = scratch.file("koi.nnue");
    {
        std::ofstream out(discovered_net, std::ios::binary);
        out << "dummy";
    }

    set_or_unset_env("KOI_NNUE_PATH", "from-env.nnue");
    const auto explicit_res =
        koi::cli::resolve_boot_nnue_path(scratch.path(), std::optional<std::string>{"from-arg.nnue"});
    koi::test::require(explicit_res.path == std::filesystem::path("from-arg.nnue") &&
                           !explicit_res.autodiscovered,
                       "explicit override must win over KOI_NNUE_PATH without autodiscovery flag");

    const auto env_res = koi::cli::resolve_boot_nnue_path(scratch.path());
    koi::test::require(env_res.path == std::filesystem::path("from-env.nnue") &&
                           !env_res.autodiscovered,
                       "KOI_NNUE_PATH must win over autodiscovery without autodiscovery flag");

    env.clear_all();
    set_or_unset_env("KOI_NNUE_AUTODETECT", "0");
    const auto disabled_res = koi::cli::resolve_boot_nnue_path(scratch.path());
    koi::test::require(disabled_res.path.empty() && !disabled_res.autodiscovered,
                       "KOI_NNUE_AUTODETECT=0 must suppress boot-time network discovery");

    env.clear_all();
    const auto auto_res = koi::cli::resolve_boot_nnue_path(scratch.path());
    koi::test::require(auto_res.path == discovered_net && auto_res.autodiscovered,
                       "discoverable koi.nnue must resolve with autodiscovered=true");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"Batch script builder", test_batch_script_builder},
        {"Inline -c/--cmd parsing and env roundtrip", test_inline_cmd_parsing_and_env_roundtrip},
        {"Evaluator and asset flags", test_evaluator_and_asset_flags},
        {"Unquoted FEN and inspection subcommands", test_unquoted_fen_and_inspection_subcommands},
        {"Bench and protocol bounds", test_bench_and_protocol_bounds},
        {"Boot NNUE path resolution", test_boot_nnue_path_resolution},
    };
    return koi::test::run_tests(tests, argc, argv);
}
