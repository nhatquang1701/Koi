#include <algorithm>
#include <array>
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

struct PerftSuiteCase {
    std::string_view name;
    std::string_view fen;
    int depth;
    std::uint64_t expected_nodes;
};

constexpr std::array<PerftSuiteCase, 6> kPerftSuite{{
    {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281},
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603},
    {"position-3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238},
    {"position-4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333},
    {"position-5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487},
    {"position-6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594},
}};

int print_usage(std::ostream& out, const int exit_code) {
    out << "usage: koi-perft [--divide] <depth> [startpos|fen [moves <move>...]]\n"
           "       koi-perft --suite\n";
    return exit_code;
}

int run_perft_suite() {
    const auto started = std::chrono::steady_clock::now();
    std::size_t passed = 0;
    std::uint64_t total_nodes = 0;
    for (const PerftSuiteCase& entry : kPerftSuite) {
        const auto parsed = koi::GameState::from_fen(entry.fen);
        if (!parsed.has_value()) {
            std::cout << "FAIL " << entry.name << " depth " << entry.depth
                      << " expected " << entry.expected_nodes << " got 0\n";
            continue;
        }
        koi::GameState state = *parsed;
        const std::uint64_t actual = koi::perft(state, entry.depth);
        total_nodes += actual;
        if (actual == entry.expected_nodes) {
            ++passed;
            std::cout << "PASS " << entry.name << " depth " << entry.depth
                      << " nodes " << actual << '\n';
        } else {
            std::cout << "FAIL " << entry.name << " depth " << entry.depth
                      << " expected " << entry.expected_nodes << " got " << actual << '\n';
        }
    }
    const auto elapsed_ms = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
               .count());
    const std::uint64_t nps = (total_nodes * 1000ULL) / static_cast<std::uint64_t>(elapsed_ms);
    std::cout << "suite " << passed << '/' << kPerftSuite.size()
              << " passed | nodes " << total_nodes
              << " | time_ms " << elapsed_ms
              << " | nps " << nps << '\n';
    return passed == kPerftSuite.size() ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return print_usage(std::cerr, 1);
    }

    bool divide = false;
    bool suite = false;
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
        if (argument == "--suite") {
            suite = true;
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

    if (suite) {
        if (divide || depth.has_value() || !fen.empty()) {
            return print_usage(std::cerr, 1);
        }
        return run_perft_suite();
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
        std::array<koi::Move, koi::kMaximumLegalMoves> moves{};
        const std::size_t move_count =
            std::min(state.legal_moves_into(moves), moves.size());
        for (std::size_t move_index = 0; move_index < move_count; ++move_index) {
            const koi::Move& move = moves[move_index];
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
