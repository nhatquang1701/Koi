#include <cctype>
#include <iostream>
#include <string>
#include <string_view>

#include "koi/game_state.hpp"

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "1.2.0"
#endif
#ifndef KOI_ENGINE_BUILD_COMMIT
#define KOI_ENGINE_BUILD_COMMIT "unknown"
#endif

namespace {

constexpr std::string_view kUsage =
    "usage: koi-replay <startpos|fen six-field-fen> [moves <coordinate>...]\n";

struct ReplayResult {
    std::string result = "*";
    std::string termination = "ongoing";
};

bool has_six_fen_fields(std::string_view fen) noexcept {
    int count = 0;
    bool in_token = false;
    for (const unsigned char ch : fen) {
        if (std::isspace(ch) != 0) {
            in_token = false;
        } else if (!in_token) {
            ++count;
            in_token = true;
        }
    }
    return count == 6;
}

std::string join_arguments(int begin, int end, char* arguments[]) {
    std::string joined;
    for (int index = begin; index < end; ++index) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += arguments[index];
    }
    return joined;
}

ReplayResult classify(const koi::GameState& state) {
    if (!state.has_legal_move()) {
        if (state.in_check()) {
            return {state.side_to_move() == koi::Color::white ? "0-1" : "1-0", "checkmate"};
        }
        return {"1/2-1/2", "stalemate"};
    }
    if (state.is_draw_by_rule()) {
        return {"1/2-1/2", "rule draw"};
    }
    return {};
}

void print_result(bool legal, const ReplayResult& result, const koi::GameState& state) {
    std::cout << "legal " << (legal ? 1 : 0) << '\n';
    std::cout << "result " << result.result << '\n';
    std::cout << "termination " << result.termination << '\n';
    std::cout << "fen " << state.fen() << '\n';
}

int usage() {
    std::cerr << kUsage;
    return 2;
}

} // namespace

int main(int argument_count, char* arguments[]) {
    if (argument_count < 2) {
        return usage();
    }
    const std::string_view first(arguments[1]);
    if (first == "-h" || first == "--help") {
        std::cout << kUsage;
        return 0;
    }
    if (first == "-v" || first == "--version") {
        std::cout << "koi-replay " << KOI_ENGINE_BUILD_VERSION << " (" << KOI_ENGINE_BUILD_COMMIT << ")\n";
        return 0;
    }

    int move_start = argument_count;
    for (int index = 1; index < argument_count; ++index) {
        if (std::string_view(arguments[index]) == "moves") {
            move_start = index;
            break;
        }
    }

    koi::GameState state;
    const std::string_view position = arguments[1];
    if (position == "startpos") {
        if ((move_start == argument_count && argument_count != 2) ||
            (move_start != argument_count && move_start != 2)) {
            return usage();
        }
    } else {
        const int fen_start = position == "fen" ? 2 : 1;
        if (fen_start >= move_start) {
            return usage();
        }
        const std::string fen = join_arguments(fen_start, move_start, arguments);
        if (!has_six_fen_fields(fen)) {
            return usage();
        }
        const auto parsed = koi::GameState::from_fen(fen);
        if (!parsed) {
            std::cerr << "invalid FEN\n";
            return 2;
        }
        state = *parsed;
    }

    bool legal = true;
    if (move_start != argument_count) {
        for (int index = move_start + 1; index < argument_count; ++index) {
            const auto move = koi::Move::parse_uci(arguments[index]);
            if (!move || !state.make_move(*move)) {
                legal = false;
                break;
            }
        }
    }

    ReplayResult result = classify(state);
    if (!legal) {
        result.termination = "illegal move";
    }
    print_result(legal, result, state);
    return 0;
}
