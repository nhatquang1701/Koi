// Dump NNUE or HCE evaluations for a position corpus as CSV.
//
// Input rows are a 6-field `FEN`, the label-corpus format `FEN;cp;best_move`,
// a 4-field FEN, or a 4-field EPD line (`4-field-fen [ops...];`). Select NNUE
// with `--nnue NET` or the clean HCE with `--hce` (no network). The echoed
// `label_cp` is the corpus label in centipawns from the side to move (empty
// for bare FEN or EPD lines); the final column is the selected evaluator's
// static evaluation for the same perspective. The FEN field never contains a
// comma, so the output is plain CSV with the FEN first.

#include "koi/classical_evaluator.hpp"
#include "koi/detail/uci_support.hpp"
#include "koi/game_state.hpp"
#include "koi/nnue.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "1.2.0"
#endif
#ifndef KOI_ENGINE_BUILD_COMMIT
#define KOI_ENGINE_BUILD_COMMIT "unknown"
#endif

namespace {

std::string strip_carriage_return(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

[[nodiscard]] constexpr bool is_ascii_space(const char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\v' || ch == '\f';
}

std::string_view trim_ascii(std::string_view value) noexcept {
    while (!value.empty() && is_ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

struct ParsedEvalInput {
    koi::GameState state;
    std::string fen;
    std::string label;
};

std::optional<ParsedEvalInput> parse_fen_or_epd_line(std::string_view line) {
    if (line.starts_with("\xEF\xBB\xBF")) {
        line.remove_prefix(3);
    }
    const std::size_t separator = line.find(';');
    const std::string_view raw_fen = trim_ascii(
        separator == std::string_view::npos ? line : line.substr(0, separator));
    std::string_view label;
    if (separator != std::string_view::npos) {
        const std::size_t start = separator + 1;
        const std::size_t end = line.find(';', start);
        label = std::string_view{line}.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
    }
    if (!raw_fen.empty()) {
        if (const auto state = koi::GameState::from_fen(raw_fen); state.has_value()) {
            return ParsedEvalInput{*state, std::string(raw_fen), std::string(label)};
        }
    }

    const auto epd = koi::detail::parse_epd_prefix(line);
    if (!epd.has_value()) {
        return std::nullopt;
    }
    return ParsedEvalInput{epd->state, epd->state.fen(), {}};
}

bool consume_value_flag(const std::string_view argument, const std::string_view flag,
                        int& index, const int argc, char** argv, std::string& destination,
                        std::string& error) {
    if (argument == flag) {
        if (index + 1 >= argc || argv[index + 1] == nullptr || argv[index + 1][0] == '\0') {
            error = "missing value for " + std::string(flag);
            return true;
        }
        destination = argv[++index];
        return true;
    }
    if (argument.size() > flag.size() && argument.starts_with(flag) &&
        argument[flag.size()] == '=') {
        const std::string_view value = argument.substr(flag.size() + 1);
        if (value.empty()) {
            error = "missing value for " + std::string(flag);
            return true;
        }
        destination = std::string(value);
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::string input_path;
    std::string output_path;
    std::string nnue_path;
    std::string limit_text;
    std::optional<std::uint64_t> limit;
    bool use_hce = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::string parse_error;
        if (consume_value_flag(argument, "--input", index, argc, argv, input_path, parse_error) ||
            consume_value_flag(argument, "--output", index, argc, argv, output_path, parse_error) ||
            consume_value_flag(argument, "--nnue", index, argc, argv, nnue_path, parse_error)) {
            if (!parse_error.empty()) {
                std::cerr << "koi-eval-features: " << parse_error << '\n';
                return 2;
            }
        } else if (consume_value_flag(argument, "--limit", index, argc, argv, limit_text,
                                      parse_error)) {
            if (!parse_error.empty()) {
                std::cerr << "koi-eval-features: " << parse_error << '\n';
                return 2;
            }
            std::uint64_t parsed_limit = 0;
            if (!koi::detail::parse_uint64(limit_text, parsed_limit) || parsed_limit == 0) {
                std::cerr << "koi-eval-features: invalid --limit '" << limit_text
                          << "' (expected positive integer)\n";
                return 2;
            }
            limit = parsed_limit;
        } else if (argument == "--hce") {
            use_hce = true;
        } else if (argument == "-h" || argument == "--help") {
            std::cout << "usage: koi-eval-features (--nnue NET | --hce) [--input FILE] [--output FILE] [--limit N]\n"
                         "  reads FEN, EPD, or FEN;cp;... rows (stdin by default) and writes\n"
                         "  CSV rows fen,label_cp,<evaluator>_cp from the side-to-move perspective\n";
            return 0;
        } else if (argument == "-v" || argument == "--version") {
            std::cout << "koi-eval-features " << KOI_ENGINE_BUILD_VERSION << " (" << KOI_ENGINE_BUILD_COMMIT << ")\n";
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
    std::uint64_t emitted = 0;
    while ((!limit.has_value() || emitted < *limit) && std::getline(*input, line)) {
        line = strip_carriage_return(std::move(line));
        if (emitted == 0 && line.starts_with("\xEF\xBB\xBF")) {
            line.erase(0, 3);
        }
        const std::string_view trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto parsed = parse_fen_or_epd_line(line);
        if (!parsed.has_value()) {
            const std::size_t separator = line.find(';');
            const std::string_view raw_fen = trim_ascii(
                separator == std::string::npos ? std::string_view{line}
                                               : std::string_view{line}.substr(0, separator));
            if (!raw_fen.empty()) {
                std::cerr << "koi-eval-features: skipped unparsable FEN " << raw_fen << '\n';
            }
            continue;
        }
        *output << parsed->fen << ',' << parsed->label << ','
                << evaluator.evaluate(parsed->state, parsed->state.side_to_move()) << '\n';
        ++emitted;
    }
    return 0;
}
