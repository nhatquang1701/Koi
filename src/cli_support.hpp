#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace koi::cli {

struct CliOptions {
    std::optional<std::string> initial_protocol;
    std::vector<std::string> commands;
    std::optional<int> bench_depth;
    std::size_t bench_hash_mb = 16;
    std::size_t bench_threads = 1;
    bool speedtest = false;
    std::size_t speedtest_threads = 1;
    std::size_t speedtest_hash_mb = 128;
    int speedtest_seconds = 3;
    bool compiler = false;
    bool display = false;
    bool fen = false;
    bool key = false;
    bool moves = false;
    bool flip = false;
    bool book = false;
    bool tb = false;
    bool nnue_info = false;
    bool eval = false;
    std::optional<std::string> see_move;
    std::optional<std::string> book_path;
    std::optional<std::string> syzygy_path;
    std::optional<std::string> nnue_info_path;
    std::optional<std::string> inspect_fen;
    std::optional<std::string> eval_fen;
    std::optional<int> perft_depth;
    std::optional<std::string> perft_fen;
};

[[nodiscard]] std::filesystem::path resolved_executable_path(int argc, char* argv[]);
[[nodiscard]] std::filesystem::path executable_directory(int argc, char* argv[]);
[[nodiscard]] bool hce_requested_from_environment() noexcept;

[[nodiscard]] std::optional<int> parse_cli_options(
    int argc, char* argv[], std::string_view version, std::string_view commit,
    int default_bench_depth, std::size_t default_bench_hash_mb,
    std::size_t default_bench_threads, bool (*is_valid_protocol)(std::string_view) noexcept,
    CliOptions& out);

[[nodiscard]] std::optional<int> run_zero_network_cli_command(
    const CliOptions& options, int argc, char* argv[], std::string_view version,
    std::string_view commit);

[[nodiscard]] std::string build_command_batch_script(const std::vector<std::string>& commands);

} // namespace koi::cli
