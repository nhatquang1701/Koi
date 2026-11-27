#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "koi/detail/uci_support.hpp"
#include "koi/game_state.hpp"
#include "koi/perft.hpp"

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "1.2.0"
#endif
#ifndef KOI_ENGINE_BUILD_COMMIT
#define KOI_ENGINE_BUILD_COMMIT "unknown"
#endif

namespace {

int print_usage(std::ostream& out, const int exit_code) {
    out << "usage: koi-perft [--divide] <depth> [startpos|fen [moves <uci>...]]\n";
    return exit_code;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return print_usage(std::cerr, 1);
    }

    bool divide = false;
    std::optional<int> depth;
    std::string fen;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "-h" || argument == "--help") {
            return print_usage(std::cout, 0);
        }
        if (argument == "-v" || argument == "--version") {
            std::cout << "koi-perft " << KOI_ENGINE_BUILD_VERSION << " (" << KOI_ENGINE_BUILD_COMMIT << ")\n";
            return 0;
        }
        if (argument == "--divide") {
            divide = true;
            continue;
        }
        if (!depth.has_value()) {
            int parsed = 0;
            const auto [end, error] =
                std::from_chars(argument.data(), argument.data() + argument.size(), parsed);
            if (error != std::errc{} || end != argument.data() + argument.size() || parsed < 0) {
                std::cerr << "depth must be a non-negative integer\n";
                return 1;
            }
            depth = parsed;
            continue;
        }
        if (!fen.empty()) {
            fen.push_back(' ');
        }
        fen.append(argument);
    }

    if (!depth.has_value()) {
        return print_usage(std::cerr, 1);
    }

    koi::GameState state = koi::GameState::startpos();
    if (!fen.empty()) {
        std::string error;
        const auto parsed = koi::detail::parse_cli_position_spec(fen, error);
        if (!parsed.has_value()) {
            std::cerr << error << '\n';
            return 2;
        }
        state = *parsed;
    }

    if (!divide) {
        std::cout << "nodes " << koi::perft(state, *depth) << '\n';
        return 0;
    }

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t total = 0;
    if (*depth == 0) {
        total = 1;
    } else {
        for (const koi::Move& move : state.legal_moves()) {
            koi::GameState child = state;
            if (!child.make_move(move)) {
                continue;
            }
            const std::uint64_t nodes = koi::perft(child, *depth - 1);
            total += nodes;
            std::cout << move.uci() << ": " << nodes << '\n';
        }
    }
    const auto elapsed_ms = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
               .count());
    const std::uint64_t nps = (total * 1000ULL) / static_cast<std::uint64_t>(elapsed_ms);
    std::cout << "\nnodes " << total << '\n'
              << "time_ms " << elapsed_ms << '\n'
              << "nps " << nps << '\n';
    return 0;
}
