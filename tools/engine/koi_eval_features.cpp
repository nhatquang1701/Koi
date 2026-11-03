// Dump NNUE or HCE evaluations for a position corpus as CSV.
//
// Input rows are either a bare `FEN` or the label-corpus format
// `FEN;cp;best_move`. Select NNUE with `--nnue NET` or the clean HCE with
// `--hce` (no network). The echoed `label_cp` is the corpus label in centipawns
// from the side to move; the final column is the selected evaluator's static
// evaluation for the same perspective. The FEN field never contains a comma, so the output is plain
// CSV with the FEN first.

#include "koi/classical_evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi/nnue.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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
    bool use_hce = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--input" && index + 1 < argc) {
            input_path = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            output_path = argv[++index];
        } else if (argument == "--hce") {
            use_hce = true;
        } else if (argument == "--nnue" && index + 1 < argc) {
            nnue_path = argv[++index];
        } else if (argument == "--help") {
            std::cout << "usage: koi-eval-features (--nnue NET | --hce) [--input FILE] [--output FILE]\n"
                         "  reads FEN or FEN;cp;... rows (stdin by default) and writes\n"
                         "  CSV rows fen,label_cp,<evaluator>_cp from the side-to-move perspective\n";
            return 0;
        } else {
            std::cerr << "koi-eval-features: unknown argument " << argument << '\n';
            return 2;
        }
    }
    if (use_hce && !nnue_path.empty()) {
        std::cerr << "koi-eval-features: --hce and --nnue are mutually exclusive\n";
        return 2;
    }
    if (nnue_path.empty() && !use_hce) {
        std::cerr << "koi-eval-features: --nnue NET or --hce is required\n";
        return 2;
    }
    koi::EvaluatorSelection selection;
    if (use_hce) {
        selection.evaluator = std::make_shared<koi::ClassicalEvaluator>();
    } else {
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

    *output << (use_hce ? "fen,label_cp,hce_cp\n" : "fen,label_cp,nnue_cp\n");
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
