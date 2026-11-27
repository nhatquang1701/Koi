#include "koi/perft.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace koi {

namespace {

std::uint64_t perft_detached(GameState& state, const int depth) {
    if (depth <= 0) {
        return 1;
    }
    if (depth == 1) {
        return static_cast<std::uint64_t>(state.legal_moves_into({}));
    }

    std::array<Move, kMaximumLegalMoves> moves;
    const std::size_t count = std::min(state.legal_moves_into(moves), moves.size());
    std::uint64_t nodes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (!state.make_move(moves[index])) {
            continue;
        }
        nodes += perft_detached(state, depth - 1);
        (void)state.unmake_move();
    }
    return nodes;
}

PerftResult perft_interruptible_detached(GameState& state, const int depth,
                                         const std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return {.nodes = 0, .cancelled = true};
    }
    if (depth <= 0) {
        return {.nodes = 1, .cancelled = false};
    }
    if (depth == 1) {
        return {
            .nodes = static_cast<std::uint64_t>(state.legal_moves_into({})),
            .cancelled = false,
        };
    }

    std::array<Move, kMaximumLegalMoves> moves;
    const std::size_t count = std::min(state.legal_moves_into(moves), moves.size());
    std::uint64_t nodes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (stop_token.stop_requested()) {
            return {.nodes = nodes, .cancelled = true};
        }
        if (!state.make_move(moves[index])) {
            continue;
        }
        const PerftResult child =
            perft_interruptible_detached(state, depth - 1, stop_token);
        (void)state.unmake_move();
        nodes += child.nodes;
        if (child.cancelled) {
            return {.nodes = nodes, .cancelled = true};
        }
    }
    return {.nodes = nodes, .cancelled = false};
}

} // namespace

std::uint64_t perft(GameState& state, int depth) {
    if (depth <= 0) {
        return 1;
    }
    if (depth == 1) {
        return static_cast<std::uint64_t>(state.legal_moves_into({}));
    }
    GameState work = state;
    work.detach_mirror();
    return perft_detached(work, depth);
}

PerftResult perft_interruptible(GameState& state, const int depth,
                                const std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return {.nodes = 0, .cancelled = true};
    }
    if (depth <= 0) {
        return {.nodes = 1, .cancelled = false};
    }
    if (depth == 1) {
        return {
            .nodes = static_cast<std::uint64_t>(state.legal_moves_into({})),
            .cancelled = false,
        };
    }
    GameState work = state;
    work.detach_mirror();
    return perft_interruptible_detached(work, depth, stop_token);
}

} // namespace koi
