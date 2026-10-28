// Dump NNUE evaluations for a position corpus as CSV.
//
// Input rows are either a bare `FEN` or the label-corpus format
// `FEN;cp;best_move`. The network is mandatory since the HCE removal
// (2026-10-28). The echoed `label_cp` is the corpus label in centipawns from
// the side to move; `nnue_cp` is the network's static evaluation for the same
// perspective. The FEN field never contains a comma, so the output is plain
// CSV with the FEN first.

#include "koi/game_state.hpp"
#include "koi/nnue.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string strip_carriage_return(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

} // namespace

int main(int argc, char** argv) {
    std::string input_path;
    std::string output_path;
    std::string nnue_path;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--input" && index + 1 < argc) {
            input_path = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            output_path = argv[++index];
        } else if (argument == "--nnue" && index + 1 < argc) {
            nnue_path = argv[++index];
        } else if (argument == "--help") {
            std::cout << "usage: koi-eval-features --nnue NET [--input FILE] [--output FILE]\n"
                         "  reads FEN or FEN;cp;... rows (stdin by default) and writes\n"
                         "  CSV rows fen,label_cp,nnue_cp from the side-to-move perspective\n";
            return 0;
        } else {
            std::cerr << "koi-eval-features: unknown argument " << argument << '\n';
            return 2;
        }
    }
    if (nnue_path.empty()) {
        std::cerr << "koi-eval-features: --nnue NET is required (the classical evaluator\n"
                     "was removed; see docs/HCE_REMOVAL_PLAN.md)\n";
        return 2;
    }
    koi::EvaluatorSelection selection;
    try {
        selection = koi::make_evaluator(std::optional<std::filesystem::path>(nnue_path));
    } catch (const std::exception& error) {
        std::cerr << "koi-eval-features: cannot load network: " << error.what() << '\n';
        return 2;
    }
    if (selection.evaluator == nullptr) {
        std::cerr << "koi-eval-features: rejected network";
        if (selection.nnue_error.has_value()) {
            std::cerr << " (" << selection.nnue_error->message << ")";
        }
        std::cerr << '\n';
        return 2;
    }

    std::ifstream file_input;
    if (!input_path.empty()) {
        file_input.open(input_path);
        if (!file_input) {
            std::cerr << "koi-eval-features: cannot open " << input_path << '\n';
            return 2;
        }
    }
    std::istream* input = input_path.empty() ? &std::cin : &file_input;

    std::ofstream file_output;
    if (!output_path.empty()) {
        file_output.open(output_path, std::ios::trunc);
        if (!file_output) {
            std::cerr << "koi-eval-features: cannot write " << output_path << '\n';
            return 2;
        }
    }
    std::ostream* output = output_path.empty() ? &std::cout : &file_output;

    *output << "fen,label_cp,nnue_cp\n";
    const koi::Evaluator& evaluator = *selection.evaluator;
    std::string line;
    while (std::getline(*input, line)) {
        line = strip_carriage_return(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t separator = line.find(';');
        const std::string_view fen = separator == std::string::npos ?
            std::string_view{line} : std::string_view{line}.substr(0, separator);
        std::string_view label;
        if (separator != std::string::npos) {
            const std::size_t start = separator + 1;
            const std::size_t end = line.find(';', start);
            label = std::string_view{line}.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
        }
        if (fen.empty()) {
            continue;
        }
        const auto state = koi::GameState::from_fen(fen);
        if (!state.has_value()) {
            std::cerr << "koi-eval-features: skipped unparsable FEN " << fen << '\n';
            continue;
        }
        *output << fen << ',' << label << ','
                << evaluator.evaluate(*state, state->side_to_move()) << '\n';
    }
    return 0;
}
