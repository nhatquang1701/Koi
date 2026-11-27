#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "koi/position.hpp"
#include "koi/classical_evaluator.hpp"
#include "koi/detail/debug_log.hpp"
#include "koi/detail/uci_support.hpp"
#include "koi/nnue.hpp"
#include "../../support/material_evaluator.hpp"
#include "../../support/nnue_synthetic.hpp"
#include "koi/time_manager.hpp"
#include "koi/uci_controller.hpp"

#include "../../support/uci_controller_test_support.hpp"

#include "koi_test_support.hpp"
#include "polyglot_book_support.hpp"

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "unknown"
#endif
#ifndef KOI_ENGINE_BUILD_COMMIT
#define KOI_ENGINE_BUILD_COMMIT "unknown"
#endif

namespace {

using koi::Position;
using koi::UciController;
using koi::test::book::BookRecord;
using koi::test::book::polyglot_move;
using koi::test::book::write_book;

struct ControllerResult {
    int exit_code;
    std::string output;
    std::string diagnostics;
};

using koi::test::require;

void test_network_overhead_estimator_tracks_gui_latency() {
    using koi::uci::derive_network_overhead_ms;
    // No latency: the clock moved exactly by Koi's search time.
    require(derive_network_overhead_ms(60'000, 0, 1'000, 59'000, 0) == 0,
            "a clean clock must not invent overhead");
    // 200 ms disappeared outside search: learn it.
    require(derive_network_overhead_ms(60'000, 0, 1'000, 58'800, 0) == 200,
            "a GUI/network stall must raise the overhead estimate");
    // Keep the largest recent observation.
    require(derive_network_overhead_ms(60'000, 0, 1'000, 58'700, 200) == 300,
            "a larger stall must dominate the estimate");
    // Clock rounding/noise must not move it, and a negative delta must decay.
    require(derive_network_overhead_ms(60'000, 0, 1'000, 58'990, 200) == 200,
            "sub-25 ms clock noise must be ignored");
    require(derive_network_overhead_ms(60'000, 0, 1'000, 59'500, 200) == 175,
            "a negative delta must decay the estimate");
    // Increment is refunded before the latency estimate.
    require(derive_network_overhead_ms(5'000, 1'000, 500, 5'500, 0) == 0,
            "the increment must be accounted for before comparing clocks");
    // Pathological stalls stay bounded.
    require(derive_network_overhead_ms(60'000, 0, 1'000, 10'000, 0) == 500,
            "the learned overhead must stay bounded");
}


std::size_t maximum_threads() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1, std::min<std::size_t>(64, hardware == 0 ? 1 : hardware));
}

ControllerResult run_controller(std::string_view transcript) {
    std::istringstream input{std::string(transcript)};
    std::ostringstream output;
    std::ostringstream diagnostics;
    auto controller_holder = koi::test::make_uci_controller(input, output, diagnostics);
    UciController& controller = *controller_holder;
    return {controller.run(), output.str(), diagnostics.str()};
}

ControllerResult run_controller_in_directory(std::string_view transcript,
                                              const std::filesystem::path& directory) {
    std::istringstream input{std::string(transcript)};
    std::ostringstream output;
    std::ostringstream diagnostics;
    UciController controller(input, output, diagnostics, koi::SearchService{
        std::make_shared<koi::test::MaterialEvaluator>()}, directory);
    return {controller.run(), output.str(), diagnostics.str()};
}

ControllerResult run_controller_with_autodiscovery(std::string_view transcript,
                                                    bool autodiscovered) {
    std::istringstream input{std::string(transcript)};
    std::ostringstream output;
    std::ostringstream diagnostics;
    UciController controller(input, output, diagnostics, koi::SearchService{
        std::make_shared<koi::test::MaterialEvaluator>()}, {}, autodiscovered);
    return {controller.run(), output.str(), diagnostics.str()};
}
koi::GameState position_after(const std::vector<std::string_view>& moves) {
    koi::GameState state = koi::GameState::startpos();
    for (const std::string_view uci : moves) {
        const auto move = koi::Move::parse_uci(uci);
        require(move.has_value() && state.make_move(*move), "test move sequence must be legal");
    }
    return state;
}

std::string join_moves(const std::vector<std::string_view>& moves) {
    std::string joined;
    for (const std::string_view move : moves) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += move;
    }
    return joined;
}

std::vector<std::string> output_lines(std::string_view output) {
    std::vector<std::string> lines;
    std::istringstream stream{std::string(output)};
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> lines_starting_with(const std::vector<std::string>& lines,
                                             std::string_view prefix) {
    std::vector<std::string> matches;
    for (const std::string& line : lines) {
        if (line.starts_with(prefix)) {
            matches.push_back(line);
        }
    }
    return matches;
}

std::size_t line_index(const std::vector<std::string>& lines, std::string_view expected) {
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index] == expected) {
            return index;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

bool is_legal_move(const Position& position, std::string_view uci) {
    for (const auto& move : position.legal_moves()) {
        if (move.uci() == uci) {
            return true;
        }
    }
    return false;
}

// A bestmove line may carry the optional ponder reply. Tests that only care
// about the played move read the first token after the keyword.
std::string first_bestmove_move(std::string_view bestmove_line) {
    std::istringstream stream{std::string(bestmove_line)};
    std::string keyword;
    std::string move;
    if (stream >> keyword >> move && keyword == "bestmove") {
        return move;
    }
    return {};
}

bool is_valid_search_info(std::string_view line) {
    std::istringstream stream{std::string(line)};
    std::string info;
    std::string depth_name;
    int depth = 0;
    std::string seldepth_name;
    int seldepth = 0;
    std::string multipv_name;
    int multipv = 0;
    std::string score_kind;
    int score = 0;
    std::string nodes_name;
    std::uint64_t nodes = 0;
    std::string nps_name;
    std::uint64_t nps = 0;
    std::string hashfull_name;
    std::uint64_t hashfull = 0;
    std::string time_name;
    std::uint64_t time = 0;
    std::string pv_name;

    std::string next_name;
    if (!(stream >> info >> depth_name >> depth >> seldepth_name >> seldepth >> multipv_name >> multipv >>
          next_name)) {
        return false;
    }
    if (next_name == "score") {
        if (!(stream >> score_kind >> score >> next_name) ||
            (score_kind != "cp" && score_kind != "mate")) {
            return false;
        }
        if (next_name == "lowerbound" || next_name == "upperbound") {
            if (!(stream >> next_name)) {
                return false;
            }
        }
    }
    // A complete selective iteration may publish legal PV progress without a
    // score; a model estimate may still include its separately identified WDL.
    if (next_name == "wdl") {
        int win = 0;
        int draw = 0;
        int loss = 0;
        if (!(stream >> win >> draw >> loss >> next_name) ||
            win < 0 || draw < 0 || loss < 0 || win + draw + loss != 1000) {
            return false;
        }
    }
    nodes_name = next_name;
    if (!(stream >> nodes >> nps_name >> nps >> hashfull_name >> hashfull >>
          time_name >> time >> pv_name)) {
        return false;
    }
    if (info != "info" || depth_name != "depth" || depth <= 0 || seldepth_name != "seldepth" ||
        seldepth < depth || multipv_name != "multipv" || multipv < 1 || multipv > 16 ||
        nodes_name != "nodes" ||
        nps_name != "nps" || hashfull_name != "hashfull" || hashfull > 1000 ||
        time_name != "time" || pv_name != "pv") {
        return false;
    }

    for (std::string move; stream >> move;) {
        if (move == "tbhits") {
            std::uint64_t hits = 0;
            std::string extra;
            return static_cast<bool>(stream >> hits) && !(stream >> extra);
        }
        if (move.size() != 4 && move.size() != 5) {
            return false;
        }
    }
    return true;
}

class FlushTrackingBuffer final : public std::stringbuf {
public:
    int sync() override {
        sync_count_.fetch_add(1, std::memory_order_relaxed);
        return std::stringbuf::sync();
    }

    [[nodiscard]] int sync_count() const noexcept {
        return sync_count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_int sync_count_ = 0;
};

class GatedInputBuffer final : public std::streambuf {
public:
    GatedInputBuffer(std::string prefix, std::string suffix)
        : prefix_(std::move(prefix)), suffix_(std::move(suffix)) {}

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    void mark_marker() {
        {
            std::lock_guard lock(mutex_);
            marker_seen_ = true;
            released_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool wait_for_marker(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return marker_seen_; });
    }

protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        if (prefix_position_ < prefix_.size()) {
            current_character_ = prefix_[prefix_position_++];
            setg(&current_character_, &current_character_, &current_character_ + 1);
            return traits_type::to_int_type(current_character_);
        }

        condition_.wait(lock, [this] { return released_; });
        if (suffix_position_ >= suffix_.size()) {
            return traits_type::eof();
        }

        current_character_ = suffix_[suffix_position_++];
        setg(&current_character_, &current_character_, &current_character_ + 1);
        return traits_type::to_int_type(current_character_);
    }

private:
    std::string prefix_;
    std::string suffix_;
    std::size_t prefix_position_ = 0;
    std::size_t suffix_position_ = 0;
    char current_character_ = '\0';
    std::mutex mutex_;
    std::condition_variable condition_;
    bool released_ = false;
    bool marker_seen_ = false;
};

class ReleaseOnMultiPvBuffer final : public std::stringbuf {
public:
    explicit ReleaseOnMultiPvBuffer(GatedInputBuffer& input) : input_(input) {}

    int sync() override {
        if (str().find(" multipv 3 ") != std::string::npos) {
            input_.mark_marker();
        }
        return std::stringbuf::sync();
    }

private:
    GatedInputBuffer& input_;
};

class ReleaseOnMultiPvOrBookBuffer final : public std::stringbuf {
public:
    explicit ReleaseOnMultiPvOrBookBuffer(GatedInputBuffer& input) : input_(input) {}

    int sync() override {
        const std::string output = str();
        if (output.find(" multipv 3 ") != std::string::npos ||
            output.find("info string book move ") != std::string::npos) {
            input_.mark_marker();
        }
        return std::stringbuf::sync();
    }

private:
    GatedInputBuffer& input_;
};

class ReleaseOnSubstringBuffer final : public std::stringbuf {
public:
    ReleaseOnSubstringBuffer(GatedInputBuffer& input, std::string needle)
        : input_(input), needle_(std::move(needle)) {}

    int sync() override {
        if (str().find(needle_) != std::string::npos) {
            input_.mark_marker();
        }
        return std::stringbuf::sync();
    }

private:
    GatedInputBuffer& input_;
    std::string needle_;
};

class ReleaseOnDepthBuffer final : public std::stringbuf {
public:
    ReleaseOnDepthBuffer(GatedInputBuffer& input, int depth) : input_(input), depth_(depth) {}

    int sync() override {
        if (str().find("info depth " + std::to_string(depth_) + " ") != std::string::npos) {
            input_.mark_marker();
        }
        return std::stringbuf::sync();
    }

private:
    GatedInputBuffer& input_;
    int depth_;
};

// Streams a transcript in stages so a gated command is sent only after the
// previous search has finished publishing its best move. Stage 0 is available
// immediately; each release_stage() call makes the next stage readable.
class StagedGatedInputBuffer final : public std::streambuf {
public:
    explicit StagedGatedInputBuffer(std::vector<std::string> stages) : stages_(std::move(stages)) {}

    void release_stage() {
        {
            std::lock_guard lock(mutex_);
            if (released_stages_ < stages_.size()) {
                ++released_stages_;
            }
        }
        condition_.notify_all();
    }

    void release_all() {
        {
            std::lock_guard lock(mutex_);
            released_stages_ = stages_.size();
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool wait_for_release_count(std::size_t count,
                                              std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this, count] { return released_stages_ >= count; });
    }

protected:
    int_type underflow() override {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (stage_index_ >= stages_.size()) {
                return traits_type::eof();
            }
            const std::string& stage = stages_[stage_index_];
            if (position_ < stage.size()) {
                current_character_ = stage[position_++];
                setg(&current_character_, &current_character_, &current_character_ + 1);
                return traits_type::to_int_type(current_character_);
            }
            if (released_stages_ > stage_index_ + 1 || released_stages_ >= stages_.size()) {
                ++stage_index_;
                position_ = 0;
                continue;
            }
            condition_.wait(lock, [this] { return released_stages_ > stage_index_ + 1; });
        }
    }

private:
    std::vector<std::string> stages_;
    std::size_t stage_index_ = 0;
    std::size_t position_ = 0;
    std::size_t released_stages_ = 1;
    char current_character_ = '\0';
    std::mutex mutex_;
    std::condition_variable condition_;
};

// Releases one input stage for every bestmove that appears in the captured
// output, so a follow-up command cannot race the search that precedes it.
class ReleaseOnBestmoveCountBuffer final : public std::stringbuf {
public:
    ReleaseOnBestmoveCountBuffer(StagedGatedInputBuffer& input, std::size_t count)
        : input_(input), expected_(count) {}

    int sync() override {
        const std::string text = str();
        std::size_t seen = 0;
        for (std::size_t position = text.find("bestmove ");
             position != std::string::npos;
             position = text.find("bestmove ", position + 1)) {
            ++seen;
        }
        while (released_ < seen && released_ < expected_) {
            ++released_;
            input_.release_stage();
        }
        return std::stringbuf::sync();
    }

private:
    StagedGatedInputBuffer& input_;
    std::size_t expected_;
    std::size_t released_ = 0;
};

class PonderGateEvaluator final : public koi::Evaluator {
public:
    explicit PonderGateEvaluator(GatedInputBuffer& input) : input_(input) {}

    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }

    [[nodiscard]] int evaluate(const koi::GameState&, koi::Color) const override {
        {
            std::lock_guard lock(mutex_);
            started_ = true;
        }
        started_condition_.notify_one();
        input_.release();
        std::unique_lock lock(mutex_);
        release_condition_.wait(lock, [this] { return released_; });
        return 0;
    }

    [[nodiscard]] bool wait_for_started(std::chrono::milliseconds timeout) const {
        std::unique_lock lock(mutex_);
        return started_condition_.wait_for(lock, timeout, [this] { return started_; });
    }

    void release() const {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        release_condition_.notify_all();
    }

private:
    GatedInputBuffer& input_;
    mutable std::mutex mutex_;
    mutable std::condition_variable started_condition_;
    mutable std::condition_variable release_condition_;
    mutable bool started_ = false;
    mutable bool released_ = false;
};

// The handshake contract lives in tests/data/uci/handshake.txt so the unit
// test and the PowerShell process test assert the same bytes.  {max_threads}
// is host dependent and {empty} keeps the trailing space of an empty string
// default visible instead of relying on editors to preserve it.
std::string expected_handshake() {
    std::ifstream input(koi::test::fixture_path("uci/handshake.txt"));
    require(input.good(), "the handshake fixture must exist");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    std::string normalized;
    normalized.reserve(text.size());
    for (const char character : text) {
        if (character != '\r') {
            normalized.push_back(character);
        }
    }

    const auto replace_all = [&normalized](std::string_view needle, std::string_view value) {
        std::size_t position = normalized.find(needle);
        while (position != std::string::npos) {
            normalized.replace(position, needle.size(), value);
            position = normalized.find(needle, position + value.size());
        }
    };
    replace_all("{max_threads}", std::to_string(maximum_threads()));
    replace_all("{empty}", "");
    replace_all("{build_version}", KOI_ENGINE_BUILD_VERSION);
    replace_all("{build_commit}", KOI_ENGINE_BUILD_COMMIT);
    return normalized;
}

void test_uci_handshake_has_identity_and_supported_options_in_order() {
    const ControllerResult result = run_controller("uci\nquit\n");
    const std::string expected = expected_handshake();

    require(result.exit_code == 0, "quit must cause a normal shutdown");
    require(result.output == expected,
            "uci response must match tests/data/uci/handshake.txt");
}

void test_evalfile_load_after_hce_reports_nnue_mode() {
    const char* network_path = std::getenv("KOI_NNUE_PATH");
    if (network_path == nullptr || *network_path == '\0') {
        return;  // direct runs without the CTest network environment
    }
    const std::string transcript =
        "uci\n"
        "setoption name EvalMode value hce\n"
        "setoption name EvalFile value " + std::string(network_path) + "\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n";
    const ControllerResult result = run_controller(transcript);
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "EvalFile after EvalMode=hce must leave the protocol clean");
    require(result.output.find("info string evaluator hce") != std::string::npos,
            "EvalMode=hce must report its mode");
    require(result.output.find("info string NNUE enabled from") != std::string::npos,
            "EvalFile must load the network after HCE mode");
    require(result.output.find("info string evaluator nnue") != std::string::npos,
            "a successful EvalFile load must restore/report NNUE mode");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    require(bestmoves.size() == 1 &&
                is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "the search after the evaluator transition must complete legally");
}

void test_eval_mode_switch_during_search_and_rejected_evalfile() {
    const ControllerResult result = run_controller(
        "uci\n"
        "position startpos\n"
        "go infinite\n"
        "setoption name EvalMode value hce\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "setoption name EvalFile value definitely-not-a-network.nnue\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "EvalMode/EvalFile transitions must keep the protocol clean");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");
    require(bestmoves.size() == 2,
            "each completed post-transition search must emit exactly one bestmove");
    for (const std::string& bestmove : bestmoves) {
        require(is_legal_move(Position{}, first_bestmove_move(bestmove)),
                "post-transition searches must return legal moves");
    }
    require(result.output.find("info string evaluator hce") != std::string::npos,
            "the HCE mode switch must be reported");
    require(result.output.find("info string EvalFile rejected") != std::string::npos,
            "a rejected EvalFile in HCE mode must be diagnosed");
}

void test_eval_mode_switches_between_hce_and_the_boot_evaluator() {
    const ControllerResult result = run_controller(
        "uci\n"
        "setoption name EvalMode value hce\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "setoption name EvalMode value nnue\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "setoption name EvalMode value nonsense\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "EvalMode changes must leave the UCI stdio clean");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");
    require(bestmoves.size() == 3,
            "each stop-bounded search must complete exactly once across EvalMode changes");
    for (const std::string& bestmove : bestmoves) {
        require(is_legal_move(Position{}, first_bestmove_move(bestmove)),
                "an EvalMode search must return a legal bestmove");
    }
    require(result.output.find("info string evaluator hce") != std::string::npos,
            "selecting HCE must report the active evaluator");
    require(result.output.find("info string evaluator nnue") != std::string::npos,
            "restoring NNUE must report the active evaluator");
    require(result.output.find("option name EvalMode type combo default nnue var nnue var hce") !=
                std::string::npos,
            "EvalMode must be advertised as an nnue/hce combo");
}

void test_syzygy_options_accept_valid_values_and_ignore_invalid_values() {
    const ControllerResult result = run_controller(
        "uci\n"
        "setoption name SyzygyPath value definitely-missing-syzygy-path\n"
        "setoption name SyzygyPath value\n"
        "setoption name SyzygyProbeDepth value 1\n"
        "setoption name SyzygyProbeDepth value 0\n"
        "setoption name SyzygyProbeDepth value 101\n"
        "setoption name SyzygyProbeLimit value 5\n"
        "setoption name SyzygyProbeLimit value 6\n"
        "setoption name SyzygyInteriorDepth value 3\n"
        "setoption name SyzygyInteriorDepth value 0\n"
        "setoption name SyzygyInteriorDepth value 101\n"
        "setoption name Syzygy50MoveRule value false\n"
        "setoption name Syzygy50MoveRule value invalid\n"
        "position fen 4k3/8/8/8/8/8/8/4K3 w - - 0 1\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    require(result.exit_code == 0, "invalid Syzygy options must not crash the controller");
    require(result.output.find("bestmove ") != std::string::npos,
            "an absent Syzygy path must fall back to ordinary search");
    require(result.diagnostics.empty(), "Syzygy option handling must keep diagnostics clean");
}

void test_legacy_strength_options_are_not_advertised() {
    const ControllerResult result = run_controller("uci\nquit\n");
    require(result.output.find("option name UCI_LimitStrength ") == std::string::npos,
            "the uncalibrated UCI_LimitStrength option must not be advertised");
    require(result.output.find("option name UCI_Elo ") == std::string::npos,
            "the uncalibrated UCI_Elo option must not be advertised");
}

void test_legacy_strength_options_are_quiet_unknown_options() {
    const ControllerResult result = run_controller(
        "setoption name UCI_LimitStrength value true\n"
        "setoption name UCI_Elo value 3190\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    require(result.exit_code == 0, "unknown legacy options must not crash the controller");
    require(lines_starting_with(output_lines(result.output), "bestmove ").size() == 1,
            "unknown legacy options must leave ordinary searches usable");
    require(result.diagnostics.empty(), "unknown legacy options must remain quiet");
}

void test_surviving_uci_strength_options_accept_valid_and_ignore_invalid_values() {
    const ControllerResult result = run_controller(
        "setoption name UCI_ShowWDL value true\n"
        "setoption name UCI_ShowWDL value invalid\n"
        "setoption name Move Overhead value 5000\n"
        "setoption name Move Overhead value 5001\n"
        "setoption name Slow Mover value 10\n"
        "setoption name Slow Mover value 9\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    require(result.exit_code == 0,
            "valid and invalid values for surviving UCI options must not crash the controller");
    require(lines_starting_with(output_lines(result.output), "bestmove ").size() == 1,
            "surviving option values must leave ordinary searches usable");
    require(result.diagnostics.empty(),
            "valid and invalid values for surviving UCI options must remain quiet");
}

void test_en_croissant_option_names_are_case_insensitive() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "en-croissant-book.bin";
    write_book(book, {{koi::GameState::startpos().polyglot_key(),
                       polyglot_move("e2", "e4"), 100, 0}});

    const ControllerResult book_file = run_controller_in_directory(
        "setoption name OwnBook value true\n"
        "setoption name bookfile value en-croissant-book.bin\n"
        "position startpos\n"
        "go depth 1\n"
        "quit\n",
        files.path());
    require(book_file.exit_code == 0 && book_file.diagnostics.empty() &&
                book_file.output.find("info string book move e2e4") != std::string::npos,
            "lowercase bookfile must be accepted by the En Croissant UCI workflow");

    const ControllerResult multipv = run_controller_in_directory(
        "setoption name BookFile value en-croissant-book.bin\n"
        "setoption name multipv value 2\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n",
        files.path());
    require(multipv.output.find("info string book move ") == std::string::npos,
            "lowercase multipv must bypass the opening book for analysis variations");

    const ControllerResult analysis = run_controller_in_directory(
        "setoption name BookFile value en-croissant-book.bin\n"
        "setoption name uci_analysemode value true\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n",
        files.path());
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(analysis.output), "bestmove ");
    require(analysis.exit_code == 0 && analysis.diagnostics.empty() &&
                analysis.output.find("info string book move ") == std::string::npos &&
                bestmoves.size() == 1 && is_legal_move(Position{}, bestmoves.front().substr(9)),
            "lowercase uci_analysemode must bypass the book and return one legal move");
}

void test_task1_wdl_output_is_optional_and_mate_scores_are_converted() {
    const ControllerResult omitted = run_controller(
        "setoption name OwnBook value false\n"
        "position startpos\n"
        "go infinite\nstop\nquit\n");
    for (const std::string& line : output_lines(omitted.output)) {
        if (line.starts_with("info depth ")) {
            require(line.find(" wdl ") == std::string::npos,
                    "WDL must be omitted when UCI_ShowWDL is disabled");
        }
    }

    GatedInputBuffer shown_input(
        "setoption name UCI_ShowWDL value true\n"
        "setoption name OwnBook value false\n"
        "position fen 7k/5Q2/6K1/8/8/8/8/8 w - - 0 1\n"
        "go infinite searchmoves f7e8\n", "stop\nquit\n");
    std::istream shown_input_stream(&shown_input);
    ReleaseOnDepthBuffer shown_output_buffer(shown_input, 1);
    std::ostream shown_output_stream(&shown_output_buffer);
    std::ostringstream shown_diagnostics;
    int shown_exit_code = -1;
    std::thread shown_controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(shown_input_stream, shown_output_stream, shown_diagnostics);
        UciController& controller = *controller_holder;
        shown_exit_code = controller.run();
    });
    const bool shown_marker = shown_input.wait_for_marker(std::chrono::seconds(5));
    shown_input.release();
    shown_controller_thread.join();
    const std::string shown_output = shown_output_buffer.str();
    bool saw_wdl = false;
    bool saw_mate_wdl = false;
    require(shown_marker && shown_exit_code == 0 && shown_diagnostics.str().empty(),
            "the mate WDL transcript must complete cleanly after its first info line");
    for (const std::string& line : output_lines(shown_output)) {
        if (!line.starts_with("info depth ")) {
            continue;
        }
        const std::size_t wdl = line.find(" wdl ");
        require(wdl != std::string::npos,
                "enabled WDL output must contain a wdl triplet: " + shown_output);
        saw_wdl = true;
        saw_mate_wdl = saw_mate_wdl || line.find(" wdl 1000 0 0") != std::string::npos;
    }
    require(saw_wdl && saw_mate_wdl,
            "mate scores must use the deterministic winning WDL conversion: " + shown_output);
}

void test_task1_hidden_debug_file_is_relative_rotated_and_off_stdio() {
    koi::test::TempDirectory files;
    const std::filesystem::path log = files.path() / "koi-debug.log";
    {
        std::ofstream existing(log, std::ios::binary | std::ios::trunc);
        std::string block(1024 * 1024, 'x');
        for (int index = 0; index < 8; ++index) {
            existing.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    for (int backup = 1; backup <= 3; ++backup) {
        std::ofstream existing(files.path() / ("koi-debug.log." + std::to_string(backup)),
                               std::ios::binary | std::ios::trunc);
        existing << "backup " << backup << '\n';
    }
    const ControllerResult result = run_controller_in_directory(
        "setoption name Debug value true\n"
        "setoption name DebugFile value relative-debug.log\n"
        "position startpos\n"
        "go depth 1\nquit\n", files.path());
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "hidden diagnostics must not write to stderr");
    require(result.output.find("debug") == std::string::npos,
            "hidden diagnostics must not write arbitrary text to UCI stdout");
    require(std::filesystem::exists(files.path() / "relative-debug.log"),
            "relative DebugFile paths must resolve beside the executable");
    std::ifstream relative_log(files.path() / "relative-debug.log", std::ios::binary);
    const std::string relative_contents((std::istreambuf_iterator<char>(relative_log)), {});
    require(relative_contents.find("command position startpos") != std::string::npos &&
                relative_contents.find("search start generation") != std::string::npos &&
                relative_contents.find("search root generation") != std::string::npos &&
                relative_contents.find("position_record ") != std::string::npos &&
                relative_contents.find("legal_digest=") != std::string::npos &&
                relative_contents.find("go_record ") != std::string::npos &&
                relative_contents.find("completion_record ") != std::string::npos,
            "debug files must include structured position, search, and completion records");

    const ControllerResult defaulted = run_controller_in_directory(
        "setoption name Debug value true\nquit\n", files.path());
    require(defaulted.exit_code == 0 && std::filesystem::exists(log),
            "Debug with an empty path must create koi-debug.log beside the executable");
    require(std::filesystem::exists(files.path() / "koi-debug.log.1"),
            "debug logs must rotate at 8 MiB");
    require(std::filesystem::exists(files.path() / "koi-debug.log.2") &&
                std::filesystem::exists(files.path() / "koi-debug.log.3"),
            "debug rotation must retain the three newest backup files");
    require(!std::filesystem::exists(files.path() / "koi-debug.log.4"),
            "debug rotation must not create a fourth backup file");
}

void test_public_option_change_emits_exactly_one_bestmove() {
    const ControllerResult result = run_controller(
        "setoption name OwnBook value false\n"
        "position startpos\n"
        "go infinite\n"
        "setoption name Move Overhead value 20\n"
        "setoption name Slow Mover value 200\n"
        "go depth 1\n"
        "stop\nquit\n");
    require(lines_starting_with(output_lines(result.output), "bestmove ").size() == 1,
            "changing public options must stop and join the replaced search once");
}

void test_task6_strength_mode_is_case_insensitive_and_cancels_active_search() {
    GatedInputBuffer input(
        "position startpos\n"
        "go infinite\n",
        "setoption name sTrEnGtHmOdE value TrUe\n"
        "stop\n"
        "quit\n");
    std::istream input_stream(&input);
    ReleaseOnDepthBuffer output_buffer(input, 1);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();

    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(output_buffer.str()), "bestmove ");
    require(marker_seen && exit_code == 0 && diagnostics.str().empty(),
            "StrengthMode must accept mixed-case UCI values while keeping the controller quiet");
    require(bestmoves.empty(),
            "changing StrengthMode must cancel and suppress the active search completion");
}

void test_book_options_emit_one_seeded_marker_and_bestmove_for_normal_play() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "controller-book.bin";
    const koi::GameState start = koi::GameState::startpos();
    write_book(book, {
        {start.polyglot_key(), polyglot_move("e2", "e4"), 1, 0},
        {start.polyglot_key(), polyglot_move("d2", "d4"), 100, 0},
    });

    const ControllerResult result = run_controller(
        "setoption name RandomSeed value 29\n"
        "setoption name OwnBook value false\n"
        "setoption name OwnBook value true\n"
        "setoption name OwnBook value invalid\n"
        "setoption name BookFile value " + book.string() + "\n"
        "setoption name BookDepth value 0\n"
        "setoption name BookDepth value 41\n"
        "position startpos\n"
        "go depth 1\n"
        "go depth 1\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> markers = lines_starting_with(lines, "info string book move ");
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(result.exit_code == 0 && result.diagnostics.empty(),
            "valid and invalid book options must leave a normal transcript clean");
    require(markers.size() == 2 && bestmoves.size() == 2,
            "each normal book hit must emit exactly one marker and one bestmove");
    require(lines.size() == 4 && lines[0] == markers[0] && lines[1] == bestmoves[0] &&
                lines[2] == markers[1] && lines[3] == bestmoves[1],
            "each book marker must be immediately followed by its one bestmove");
    require(markers[0] == markers[1], "a nonzero RandomSeed must choose the same book move at the same root");
    require(markers[0].starts_with("info string book move ") && markers[0].ends_with(" depth 0"),
            "a valid unlimited BookDepth must survive an invalid replacement");
    const std::string move = markers[0].substr(std::string("info string book move ").size(), 4);
    require(bestmoves[0] == "bestmove " + move && bestmoves[1] == "bestmove " + move,
            "every book bestmove must be the legal selected move");
}

// A book shipped beside the executable is used without the GUI enabling
// OwnBook, but an explicit `OwnBook false` must still win.
void test_opening_book_is_auto_detected() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "book.bin";
    const koi::GameState start = koi::GameState::startpos();
    write_book(book, {{start.polyglot_key(), polyglot_move("e2", "e4"), 1, 0}});

    const ControllerResult automatic = run_controller_in_directory(
        "setoption name BookFile value book.bin\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n",
        files.path());
    const std::vector<std::string> automatic_lines = output_lines(automatic.output);
    require(automatic.exit_code == 0 && automatic.diagnostics.empty(),
            "a default search with an available book must remain protocol-clean");
    require(lines_starting_with(automatic_lines, "info string book move ").size() == 1,
            "an available book must be used when OwnBook was never set");
    require(lines_starting_with(automatic_lines, "info string book detected: ").size() == 1,
            "the automatic book selection must be announced once");

    const ControllerResult refused = run_controller_in_directory(
        "setoption name BookFile value book.bin\n"
        "setoption name OwnBook value false\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n",
        files.path());
    const std::vector<std::string> refused_lines = output_lines(refused.output);
    require(refused.exit_code == 0 && refused.diagnostics.empty(),
            "an explicit OwnBook false search must remain protocol-clean");
    require(lines_starting_with(refused_lines, "info string book move ").empty(),
            "an explicit OwnBook false must disable the auto-detected book");
    require(lines_starting_with(refused_lines, "bestmove ").size() == 1,
            "the search must still return one bestmove");
}

void test_invalid_book_is_not_announced_as_detected() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "book.bin";
    {
        // 40 bytes: not a whole number of 16-byte Polyglot records, so the
        // loader will reject it and detection must not claim otherwise.
        std::ofstream stream(book, std::ios::binary);
        const std::string bytes(40, '\0');
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    const ControllerResult result = run_controller_in_directory(
        "setoption name BookFile value book.bin\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n",
        files.path());
    const std::vector<std::string> lines = output_lines(result.output);
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "a rejected book must leave the transcript protocol-clean");
    require(lines_starting_with(lines, "info string book detected: ").empty(),
            "a file the loader rejects must not be announced as a detected book");
    require(lines_starting_with(lines, "info string book move ").empty(),
            "a rejected book must not produce book moves");
    require(lines_starting_with(lines, "bestmove ").size() == 1,
            "the search must still return exactly one bestmove");
}

void test_book_random_option_accepts_valid_values_and_ignores_invalid_values() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "deterministic-controller-book.bin";
    const koi::GameState start = koi::GameState::startpos();
    write_book(book, {
        {start.polyglot_key(), polyglot_move("e2", "e4"), 1, 0},
        {start.polyglot_key(), polyglot_move("d2", "d4"), 100, 0},
    });

    const ControllerResult result = run_controller(
        "setoption name BookFile value " + book.string() + "\n"
        "setoption name OwnBook value true\n"
        "setoption name BookRandom value false\n"
        "setoption name BookRandom value invalid\n"
        "position startpos\n"
        "go depth 1\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> markers = lines_starting_with(lines, "info string book move ");
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(result.exit_code == 0 && result.diagnostics.empty(),
            "valid and invalid BookRandom values must remain protocol-clean");
    require(markers.size() == 1 && markers[0] == "info string book move d2d4 depth 0",
            "BookRandom false and an invalid replacement must retain deterministic selection");
    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove d2d4",
            "deterministic book selection must emit the selected legal move");
}

void test_book_safety_options_reject_poisoned_moves_and_fallback_to_search() {
    koi::test::TempDirectory files;
    const auto unsafe_position = koi::GameState::from_fen(
        "k3r3/8/8/8/4Q3/8/8/K7 w - - 0 1");
    require(unsafe_position.has_value(), "the book-safety fixture FEN must be valid");
    const koi::GameState unsafe = *unsafe_position;
    const std::filesystem::path book = files.path() / "unsafe-controller-book.bin";
    write_book(book, {{unsafe.polyglot_key(), polyglot_move("e4", "e3"), 100, 0}});

    const ControllerResult safe = run_controller(
        "setoption name BookFile value " + book.string() + "\n"
        "setoption name OwnBook value true\n"
        "setoption name BookSafety value true\n"
        "setoption name BookSafetyDepth value 2\n"
        "position fen k3r3/8/8/8/4Q3/8/8/K7 w - - 0 1\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    const ControllerResult disabled = run_controller(
        "setoption name BookFile value " + book.string() + "\n"
        "setoption name OwnBook value true\n"
        "setoption name BookSafety value false\n"
        "setoption name BookSafetyDepth value 9\n"
        "position fen k3r3/8/8/8/4Q3/8/8/K7 w - - 0 1\n"
        "go depth 1\n"
        "quit\n");

    require(safe.exit_code == 0 && safe.diagnostics.empty() &&
                lines_starting_with(output_lines(safe.output), "info string book move ").empty(),
            "BookSafety must reject a poisoned move and fall back silently to search");
    require(disabled.exit_code == 0 && disabled.diagnostics.empty() &&
                disabled.output.find("info string book move e4e3") != std::string::npos &&
                disabled.output.find("bestmove e4e3") != std::string::npos,
            "BookSafety=false must preserve the legal configured book move");
}

void test_book_fallback_and_analysis_style_commands_search_without_markers() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "controller-book.bin";
    const koi::GameState start = koi::GameState::startpos();
    write_book(book, {{start.polyglot_key(), polyglot_move("e2", "e4"), 1, 0}});

    const std::filesystem::path malformed = files.path() / "malformed.bin";
    {
        std::ofstream stream(malformed, std::ios::binary);
        stream.put('x');
    }
    const ControllerResult missing_fallback = run_controller(
        "setoption name BookFile value " + (files.path() / "missing.bin").string() + "\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    const ControllerResult malformed_fallback = run_controller(
        "setoption name BookFile value " + malformed.string() + "\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    for (const ControllerResult* fallback : {&missing_fallback, &malformed_fallback}) {
        const std::vector<std::string> lines = output_lines(fallback->output);
        const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");
        require(fallback->exit_code == 0 && fallback->diagnostics.empty(),
                "unusable books must leave normal-play fallback transcripts clean");
        require(lines_starting_with(lines, "info string book move ").empty(),
                "a missing or malformed book must silently fall back to search");
        require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
                "unusable book fallback must emit one legal normal-search bestmove");
    }

    const ControllerResult bypass = run_controller(
        "setoption name BookFile value " + book.string() + "\n"
        "setoption name UCI_AnalyseMode value true\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "setoption name UCI_AnalyseMode value false\n"
        "go infinite\n"
        "stop\n"
        "go ponder depth 1\n"
        "stop\n"
        "go searchmoves e2e4 depth 1\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> markers =
        lines_starting_with(output_lines(bypass.output), "info string book move ");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(bypass.output), "bestmove ");
    require(bypass.exit_code == 0 && bypass.diagnostics.empty(),
            "analysis-style book bypass commands must leave stderr clean");
    require(markers.empty(), "analysis, infinite, ponder, and searchmoves commands must bypass the book");
    require(bestmoves.size() == 4 && bestmoves.back() == "bestmove e2e4",
            "every bypassed command must preserve its exactly-once search completion");
}

void test_multipv_search_bypasses_a_matching_book_without_analysis_mode() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "multipv-book.bin";
    const koi::GameState start = koi::GameState::startpos();
    write_book(book, {{start.polyglot_key(), polyglot_move("e2", "e4"), 1, 0}});

    GatedInputBuffer input(
        "setoption name BookFile value " + book.string() + "\n"
        "setoption name MultiPV value 3\n"
        "position startpos\n"
        "go depth 1\n",
        "stop\nquit\n");
    std::istream input_stream(&input);
    ReleaseOnMultiPvOrBookBuffer output_buffer(input);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();

    const std::vector<std::string> lines = output_lines(output_buffer.str());
    const std::vector<std::string> infos = lines_starting_with(lines, "info depth ");
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(marker_seen, "MultiPV search must reach its third variation rather than stop at a book hit");
    require(exit_code == 0 && diagnostics.str().empty(),
            "a normal MultiPV transcript with a matching book must remain protocol-clean");
    require(lines_starting_with(lines, "info string book move ").empty(),
            "MultiPV greater than one must bypass a matching opening-book entry");
    bool saw_multipv_one = false;
    bool saw_multipv_two = false;
    bool saw_multipv_three = false;
    for (const std::string& info : infos) {
        require(is_valid_search_info(info), "MultiPV book bypass must emit valid search info");
        saw_multipv_one = saw_multipv_one || info.find(" multipv 1 ") != std::string::npos;
        saw_multipv_two = saw_multipv_two || info.find(" multipv 2 ") != std::string::npos;
        saw_multipv_three = saw_multipv_three || info.find(" multipv 3 ") != std::string::npos;
    }
    require(saw_multipv_one && saw_multipv_two && saw_multipv_three,
            "MultiPV book bypass must retain all requested ranked search variations");
    require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "MultiPV book bypass must retain exactly one legal search completion");
}

void test_ponderhit_keeps_the_entire_ponder_workflow_out_of_the_book() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "ponder-book.bin";
    const koi::GameState start = koi::GameState::startpos();
    write_book(book, {{start.polyglot_key(), polyglot_move("e2", "e4"), 1, 0}});

    GatedInputBuffer input(
        "setoption name BookFile value " + book.string() + "\n"
        "position startpos\n"
        "go ponder depth 2\n",
        "ponderhit\n"
        "stop\n"
        "quit\n");
    std::istream input_stream(&input);
    ReleaseOnDepthBuffer output_buffer(input, 2);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();

    const std::vector<std::string> lines = output_lines(output_buffer.str());
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(marker_seen, "ponderhit book bypass must observe a completed ponder PV");
    require(exit_code == 0 && diagnostics.str().empty(),
            "a ponderhit transcript must shut down cleanly");
    require(lines_starting_with(lines, "info string book move ").empty(),
            "ponderhit must not turn a ponder workflow into a book search");
    std::vector<koi::Move> expected_pv;
    for (const std::string& line : lines) {
        if (!line.starts_with("info depth ") || line.find(" multipv 1 ") == std::string::npos) {
            continue;
        }
        const std::size_t pv_start = line.find(" pv ");
        if (pv_start == std::string::npos) {
            continue;
        }
        std::istringstream pv_stream(line.substr(pv_start + 4));
        for (std::string move; pv_stream >> move;) {
            const auto parsed = koi::Move::parse_uci(move);
            require(parsed.has_value(), "ponder book bypass PV must contain coordinate moves");
            expected_pv.push_back(*parsed);
        }
        if (expected_pv.size() >= 2) {
            break;
        }
        expected_pv.clear();
    }
    require(expected_pv.size() >= 2, "ponderhit book bypass must retain an expected reply");
    const auto continued_bestmove = bestmoves.size() == 1 ?
        koi::Move::parse_uci(first_bestmove_move(bestmoves[0])) : std::nullopt;
    require(bestmoves.size() == 1 && continued_bestmove.has_value() &&
                Position{}.is_legal(*continued_bestmove),
            "ponderhit must retain one legal search completion outside the book");
}

void test_book_depth_boundaries_and_invalid_values_preserve_the_previous_limit() {
    koi::test::TempDirectory files;
    const std::filesystem::path book = files.path() / "depth-book.bin";
    const std::vector<std::string_view> first_fifteen{
        "g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8",
        "g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1",
    };
    const std::vector<std::string_view> first_sixteen{
        "g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8",
        "g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8",
    };
    const koi::GameState ply_fifteen = position_after(first_fifteen);
    const koi::GameState ply_sixteen = position_after(first_sixteen);
    write_book(book, {
        {ply_fifteen.polyglot_key(), polyglot_move("e7", "e5"), 1, 0},
        {ply_sixteen.polyglot_key(), polyglot_move("e2", "e4"), 1, 0},
    });

    const ControllerResult result = run_controller(
        "setoption name BookFile value " + book.string() + "\n"
        "setoption name OwnBook value true\n"
        "setoption name BookDepth value 16\n"
        "position startpos moves " + join_moves(first_fifteen) + "\n"
        "go depth 1\n"
        "position startpos moves " + join_moves(first_sixteen) + "\n"
        "go depth 1\n"
        "stop\n"
        "setoption name BookDepth value 0\n"
        "position startpos moves " + join_moves(first_sixteen) + "\n"
        "go depth 1\n"
        "setoption name BookDepth value 41\n"
        "position startpos moves " + join_moves(first_sixteen) + "\n"
        "go depth 1\n"
        "quit\n");
    const std::vector<std::string> markers =
        lines_starting_with(output_lines(result.output), "info string book move ");

    require(result.exit_code == 0 && result.diagnostics.empty(),
            "book-depth boundary commands must leave a clean transcript");
    require(markers.size() == 3, "BookDepth 16 must exclude ply 16 while zero remains unlimited");
    require(markers[0] == "info string book move e7e5 depth 15",
            "BookDepth 16 must still use a book move at ply 15");
    require(markers[1] == "info string book move e2e4 depth 16" &&
                markers[2] == "info string book move e2e4 depth 16",
            "unlimited BookDepth and an invalid replacement must both retain ply-16 book use");
}

void test_book_option_changes_suppress_active_search_generations() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go infinite\n"
        "setoption name OwnBook value false\n"
        "go infinite\n"
        "setoption name BookRandom value true\n"
        "go infinite\n"
        "setoption name BookRandom value invalid\n"
        "go infinite\n"
        "setoption name BookFile value missing.bin\n"
        "go infinite\n"
        "setoption name BookDepth value 0\n"
        "go infinite\n"
        "stop\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(result.exit_code == 0 && result.diagnostics.empty(),
            "book option changes during search must leave a clean transcript");
    require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "each book option change must suppress stale searches and leave one current completion");
}

void test_hash_options_preserve_the_contract_and_never_advertise_threads() {
    const ControllerResult result = run_controller(
        "uci\n"
        "setoption name Hash value 1\n"
        "setoption name Hash value 0\n"
        "setoption name Hash value 4097\n"
        "setoption name Clear Hash\n"
        "isready\n"
        "quit\n");

    require(result.exit_code == 0, "Hash and Clear Hash commands must leave the controller usable");
    require(result.output.find("option name Hash type spin default 512 min 1 max 4096\n") != std::string::npos,
            "the Hash option must retain its documented default and bounds");
    require(result.output.find("option name Clear Hash type button\n") != std::string::npos,
            "Clear Hash must remain a UCI button option");
    require(result.output.find("option name Threads type spin default 1 min 1 max ") != std::string::npos,
            "Threads must remain a valid advertised UCI option");
    require(result.output.find("option name UCI_AnalyseMode type check default false\n") != std::string::npos,
            "UCI_AnalyseMode must be advertised for Lucas Chess");
    require(result.output.find("option name MultiPV type spin default 1 min 1 max 16\n") != std::string::npos,
            "MultiPV must retain its Lucas Chess range");
    require(result.output.find("option name Ponder type check default false\n") != std::string::npos,
            "Ponder must be advertised for Lucas Chess");
    require(result.output.ends_with("readyok\n"), "Hash option changes and Clear Hash must not disrupt isready");
}

void test_threads_option_accepts_valid_values_and_ignores_invalid_values() {
    const std::string valid_threads = std::to_string(maximum_threads());
    const ControllerResult result = run_controller(
        "setoption name Threads value " + valid_threads + "\n"
        "setoption name Threads value 0\n"
        "setoption name Threads value 65\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    require(result.exit_code == 0, "valid and invalid Threads options must not crash the controller");
    require(bestmoves.size() == 1 && bestmoves[0].starts_with("bestmove "),
            "the accepted option must leave the controller able to search");
}

void test_threads_and_move_overhead_changes_suppress_the_active_generation() {
    const std::string valid_threads = std::to_string(std::min<std::size_t>(2, maximum_threads()));
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go infinite\n"
        "setoption name Threads value " + valid_threads + "\n"
        "go infinite\n"
        "setoption name Move Overhead value 50\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    require(bestmoves.size() == 1, "changing Threads or Move Overhead must suppress the replaced search result");
    require(is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "the surviving search after an option change must return a legal move");
}

void test_lucas_analysis_options_accept_valid_values_ignore_invalid_values_and_emit_multipv() {
    GatedInputBuffer input(
        "setoption name UCI_AnalyseMode value TRUE\n"
        "setoption name MultiPV value 3\n"
        "setoption name Ponder value TrUe\n"
        "setoption name MultiPV value 0\n"
        "setoption name MultiPV value 17\n"
        "setoption name Ponder value maybe\n"
        "position startpos\n"
        "go depth 1\n",
        "stop\nquit\n");
    std::istream input_stream(&input);
    ReleaseOnMultiPvBuffer output_buffer(input);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();

    const std::vector<std::string> lines = output_lines(output_buffer.str());
    const std::vector<std::string> infos = lines_starting_with(lines, "info ");
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(marker_seen, "MultiPV must produce an observable third principal variation");
    require(exit_code == 0 && diagnostics.str().empty(),
            "valid and invalid Lucas analysis options must leave the controller usable and quiet");
    require(infos.size() >= 3, "MultiPV must emit at least three info lines");
    bool saw_multipv_one = false;
    bool saw_multipv_two = false;
    bool saw_multipv_three = false;
    for (const std::string& info : infos) {
        require(is_valid_search_info(info),
                "every search info line must include a valid multipv field between seldepth and score");
        saw_multipv_one = saw_multipv_one || info.find(" multipv 1 ") != std::string::npos;
        saw_multipv_two = saw_multipv_two || info.find(" multipv 2 ") != std::string::npos;
        saw_multipv_three = saw_multipv_three || info.find(" multipv 3 ") != std::string::npos;
    }
    require(saw_multipv_one && saw_multipv_two && saw_multipv_three,
            "MultiPV must emit distinct ranked lines for all three requested variations");
    require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "Lucas analysis options must retain exactly one legal bestmove");
}

void test_lucas_analysis_option_changes_suppress_the_active_generation() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go infinite\n"
        "setoption name UCI_AnalyseMode value true\n"
        "go infinite\n"
        "setoption name MultiPV value 3\n"
        "go infinite\n"
        "setoption name Ponder value true\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "changing Lucas analysis options must suppress replaced search generations");
}

void test_ponderhit_continues_the_ponder_search_in_place() {
    koi::test::TempDirectory files;
    const std::filesystem::path log_path = files.path() / "ponderhit-continuation.log";
    GatedInputBuffer input(
        "setoption name Debug value true\n"
        "setoption name DebugFile value " + log_path.string() + "\n"
        "position startpos\n"
        "go ponder searchmoves e2e4 depth 2\n",
        "ponderhit\n"
        "stop\n"
        "quit\n");
    std::istream input_stream(&input);
    ReleaseOnDepthBuffer output_buffer(input, 2);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        UciController controller(input_stream, output, diagnostics,
                                 koi::SearchService{std::make_shared<koi::test::MaterialEvaluator>()},
                                 files.path());
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();
    const std::vector<std::string> lines = output_lines(output_buffer.str());
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(marker_seen, "ponderhit continuation must observe a completed two-move ponder PV");
    require(exit_code == 0 && diagnostics.str().empty(),
            "a ponderhit continuation transcript must shut down normally");
    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove e2e4",
            "a ponderhit must continue the filtered search root instead of restarting it");

    std::ifstream log_stream(log_path);
    const std::string log((std::istreambuf_iterator<char>(log_stream)),
                          std::istreambuf_iterator<char>());
    require(log.find("\"event\":\"ponderhit\"") != std::string::npos &&
                log.find("\"continued\":true") != std::string::npos,
            "a ponderhit must be recorded as an in-place continuation");
    std::size_t search_starts = 0;
    for (std::size_t position = log.find("search start generation");
         position != std::string::npos;
         position = log.find("search start generation", position + 1)) {
        ++search_starts;
    }
    require(search_starts == 1, "a ponderhit continuation must not launch a second search");
}

void test_large_hash_option_is_capped_without_crashing_the_controller() {
    koi::HashMemoryPolicy policy;
    policy.memory_provider = [] {
        return koi::HashMemorySnapshot{
            128ULL * 1024ULL * 1024ULL,
            96ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL};
    };

    std::istringstream input(
        "setoption name Hash value 4096\n"
        "isready\n"
        "position startpos\n"
        "go depth 1\n"
        "quit\n");
    std::ostringstream output;
    std::ostringstream diagnostics;
    UciController controller(input, output, diagnostics,
                              koi::SearchService{std::make_shared<koi::test::MaterialEvaluator>(),
                                                 std::move(policy)});
    require(controller.run() == 0, "a capped large hash request must not crash the controller");
    require(output.str().find("info string hash requested 4096 MB effective ") != std::string::npos,
            "a reduced large hash request must be reported as valid UCI info");
    require(output.str().find("readyok\n") != std::string::npos,
            "a capped large hash request must leave isready usable");
}

void test_hash_allocation_failure_preserves_table_and_controller() {
    // Allow the initial table allocation, then fail every resize: the Hash
    // option must report allocation_failed, keep the previous table, and leave
    // the controller able to search normally.
    auto allocations = std::make_shared<int>(0);
    koi::HashMemoryPolicy policy;
    policy.allocation_failure = [allocations](std::size_t) {
        return (*allocations)++ > 0;
    };

    std::istringstream input(
        "setoption name Hash value 64\n"
        "isready\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    std::ostringstream output;
    std::ostringstream diagnostics;
    UciController controller(input, output, diagnostics,
                              koi::SearchService{std::make_shared<koi::test::MaterialEvaluator>(),
                                                 std::move(policy)});
    require(controller.run() == 0,
            "a failed hash allocation must not crash the controller");
    require(diagnostics.str().empty(),
            "a failed hash allocation must not write diagnostics");
    require(output.str().find("reason allocation_failed") != std::string::npos,
            "a failed hash allocation must be reported as allocation_failed");
    require(output.str().find("readyok\n") != std::string::npos,
            "isready must stay usable after a failed hash allocation");
    require(lines_starting_with(output_lines(output.str()), "bestmove ").size() == 1,
            "the controller must still complete one search after a failed hash allocation");
}

void test_immediate_ponderhit_without_observed_reply_still_answers() {
    GatedInputBuffer input(
        "position startpos\n"
        "go ponder depth 2\n",
        "ponderhit\n"
        "stop\n"
        "quit\n");
    auto evaluator = std::make_shared<PonderGateEvaluator>(input);
    std::istream input_stream(&input);
    std::ostringstream output;
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        UciController controller(input_stream, output, diagnostics,
                                 koi::SearchService{evaluator});
        exit_code = controller.run();
    });

    require(evaluator->wait_for_started(std::chrono::seconds(5)),
            "the ponder test must reach evaluation before releasing ponderhit");
    evaluator->release();
    controller_thread.join();

    require(exit_code == 0 && diagnostics.str().empty(),
            "an immediate ponderhit must shut down cleanly");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(output.str()), "bestmove ");
    require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "a ponderhit always obliges the engine to answer: even without an observed expected "
            "reply it must emit exactly one legal bestmove instead of leaving the GUI waiting");
}

void test_quit_and_eof_suppress_a_ponderhit_replacement_search() {
    const ControllerResult quit = run_controller(
        "position startpos\n"
        "go ponder depth 2\n"
        "ponderhit\n"
        "quit\n");
    const ControllerResult eof = run_controller(
        "position startpos\n"
        "go ponder depth 2\n"
        "ponderhit\n");

    require(quit.exit_code == 0 && eof.exit_code == 0,
            "quit and EOF must both shut down a restarted ponder search cleanly");
    require(lines_starting_with(output_lines(quit.output), "bestmove ").empty(),
            "quit must suppress a ponderhit replacement search's late bestmove");
    require(lines_starting_with(output_lines(eof.output), "bestmove ").empty(),
            "EOF must suppress a ponderhit replacement search's late bestmove");
}

void test_idle_ponderhit_is_quiet() {
    const ControllerResult result = run_controller("ponderhit\nquit\n");

    require(result.exit_code == 0, "an idle ponderhit transcript must shut down normally");
    require(result.output.empty(), "ponderhit without an active ponder search must be quiet");
}

void test_stopping_ponder_search_emits_one_legal_bestmove() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go ponder depth 2\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(result.exit_code == 0, "ponder stop transcript must shut down normally");
    require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
            "stopping ponder must emit exactly one legal bestmove");
}

void test_stopping_ponder_search_preserves_the_searchmoves_root_filter() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go ponder searchmoves e2e4\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove e2e4",
            "stopping a filtered ponder search must retain its only legal root move");
}

void test_stopping_terminal_ponder_search_emits_0000() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go ponder\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove 0000",
            "stopping a terminal ponder search must emit exactly one bestmove 0000");
}

void test_isready_writes_readyok() {
    const ControllerResult result = run_controller("isready\nquit\n");

    require(result.output == "readyok\n", "isready must write readyok");
}

void test_deterministic_search_repeats_the_best_move_with_compatibility_seed() {
    // Each search must finish on its own before the next command is sent:
    // interrupting a search with stop lets the scheduler pick between the
    // completed and aborted root fallbacks, which is not a determinism claim.
    StagedGatedInputBuffer input({
        "setoption name RandomSeed value 42\n"
        "position startpos\n"
        "go depth 2\n",
        "stop\n"
        "setoption name RandomSeed value 42\n"
        "position startpos\n"
        "go depth 2\n",
        "stop\n"
        "quit\n"});
    std::istream input_stream(&input);
    ReleaseOnBestmoveCountBuffer output_buffer(input, 2);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool both_searches_completed =
        input.wait_for_release_count(3, std::chrono::seconds(15));
    if (!both_searches_completed) {
        input.release_all();
    }
    controller_thread.join();

    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(output_buffer.str()), "bestmove ");
    const Position initial;

    require(both_searches_completed && exit_code == 0 && diagnostics.str().empty(),
            "both deterministic searches must complete before the transcript ends");
    require(bestmoves.size() == 2, "each go command must emit exactly one bestmove");
    require(bestmoves[0] == bestmoves[1], "deterministic search must repeat from the same root");
    require(is_legal_move(initial, first_bestmove_move(bestmoves[0])),
            "the deterministic start-position move must be legal");
}

void test_startpos_and_fen_move_lists_define_the_search_root() {
    const ControllerResult result = run_controller(
        "position startpos moves e2e4 e7e5 g1f3\n"
        "go depth 1\n"
        "stop\n"
        "position fen r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1 moves e1g1\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    Position after_startpos;
    Position after_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");

    require(after_startpos.apply_uci("e2e4") && after_startpos.apply_uci("e7e5") &&
                after_startpos.apply_uci("g1f3"),
            "test fixture start-position moves must be legal");
    require(after_fen.apply_uci("e1g1"), "test fixture FEN move must be legal");
    require(bestmoves.size() == 2, "both go commands must receive one bestmove");
    require(is_legal_move(after_startpos, first_bestmove_move(bestmoves[0])),
            "startpos move list must be applied before searching");
    require(is_legal_move(after_fen, first_bestmove_move(bestmoves[1])),
            "FEN move list must be applied before searching");
}

void test_all_go_limits_and_malformed_values_are_accepted_without_crashing() {
    const ControllerResult result = run_controller(
        "position fen rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2\n"
        "go depth 1\nstop\n"
        "go nodes 0\nstop\n"
        "go movetime 0\nstop\n"
        "go wtime 0 btime 0 winc 0 binc 0 movestogo 1\nstop\n"
        "go infinite\nstop\n"
        "go depth bad nodes -1 movetime -1 wtime bad btime -1 winc nope binc -4 movestogo 0\nstop\n"
        "go\nstop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    const Position en_passant("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");

    require(result.exit_code == 0, "valid and malformed go tokens must not crash the controller");
    require(bestmoves.size() == 7, "every stopped go variant must emit exactly one bestmove");
    for (const std::string& bestmove : bestmoves) {
        require(is_legal_move(en_passant, first_bestmove_move(bestmove)),
                "every supported-limit search must retain a legal root move");
        // bestmove may carry the optional ponder reply; when present it must be
        // a coordinate move, never a stray token.
        std::istringstream line(bestmove);
        std::string keyword;
        std::string move;
        std::string ponder_keyword;
        std::string ponder_move;
        require(line >> keyword >> move && keyword == "bestmove",
                "every supported-limit search must emit a well-formed bestmove line");
        if (line >> ponder_keyword) {
            require(ponder_keyword == "ponder" && line >> ponder_move,
                    "the optional bestmove reply must use the ponder keyword");
        }
    }
}

void test_go_limit_parser_maps_each_supported_limit_exactly() {
    using namespace std::chrono_literals;

    const koi::SearchLimits limits = koi::uci::parse_go_limits(
        "depth 7 nodes 18446744073709551615 movetime 0 "
        "wtime 600000 btime 123456 winc 3500 binc 17 movestogo 40");

    require(limits.depth == 7, "depth must map to SearchLimits::depth");
    require(limits.nodes == std::numeric_limits<std::uint64_t>::max(),
            "nodes must preserve the full uint64 range");
    require(limits.movetime == 0ms, "movetime must map to milliseconds exactly");
    require(limits.white_clock.has_value() && limits.white_clock->remaining == 600000ms &&
                limits.white_clock->increment == 3500ms,
            "wtime and winc must map to the white clock exactly");
    require(limits.black_clock.has_value() && limits.black_clock->remaining == 123456ms &&
                limits.black_clock->increment == 17ms,
            "btime and binc must map to the black clock exactly");
    require(limits.moves_to_go == 40, "movestogo must map to SearchLimits::moves_to_go");
    require(!limits.infinite, "ordinary limits must not enable infinite search");
}

void test_go_mate_depth_combines_with_explicit_depth() {
    const koi::SearchLimits mate_is_tighter =
        koi::uci::parse_go_limits("mate 2 depth 5");
    require(mate_is_tighter.mate == 2 && mate_is_tighter.depth == 3,
            "mate 2 must cap a deeper explicit depth at three plies");

    const koi::SearchLimits depth_is_tighter =
        koi::uci::parse_go_limits("depth 1 mate 2");
    require(depth_is_tighter.mate == 2 && depth_is_tighter.depth == 1,
            "a shallower explicit depth must remain the effective limit");
}

void test_go_limit_parser_uses_a_scaled_bare_go_fallback_and_independent_clocks() {
    using namespace std::chrono_literals;

    const koi::SearchLimits bare = koi::uci::parse_go_limits("");
    require(bare.movetime == 250ms, "bare go must use the approved 250 ms movetime fallback");
    require(!bare.depth.has_value(), "bare go must not invent a depth limit");

    const koi::TimeManager normal(bare, koi::Color::white, 100);
    require(normal.time_budget().has_value() && *normal.time_budget() <= 250ms,
            "the bare-go fallback must remain a bounded time budget");

    const koi::SearchLimits white_only = koi::uci::parse_go_limits("wtime 1000 winc 25");
    require(white_only.white_clock.has_value() && white_only.white_clock->remaining == 1000ms &&
                white_only.white_clock->increment == 25ms && !white_only.black_clock.has_value(),
            "white clock fields must be accepted without a black clock");
    require(koi::TimeManager(white_only, koi::Color::white).time_budget().has_value() &&
                !koi::TimeManager(white_only, koi::Color::black).time_budget().has_value(),
            "a one-sided clock must budget only for the side whose clock was supplied");

    const koi::SearchLimits black_only = koi::uci::parse_go_limits("btime 1000 binc 25");
    require(black_only.black_clock.has_value() && black_only.black_clock->remaining == 1000ms &&
                black_only.black_clock->increment == 25ms && !black_only.white_clock.has_value(),
            "black clock fields must be accepted without a white clock");
    require(koi::TimeManager(black_only, koi::Color::black).time_budget().has_value() &&
                !koi::TimeManager(black_only, koi::Color::white).time_budget().has_value(),
            "the black one-sided clock must budget only for Black");
}

void test_go_limit_parser_supports_lucas_root_options_and_value_defaults() {
    const auto limits = koi::uci::parse_go_limits(
        "ponder searchmoves e2e4 g1f3 depth 5");
    require(limits.ponder, "ponder must be parsed");
    require(limits.search_moves_specified && limits.search_moves.size() == 2,
            "searchmoves must preserve both coordinate moves");
    require(limits.search_moves[0].uci() == "e2e4" && limits.search_moves[1].uci() == "g1f3",
            "searchmoves must parse coordinate moves through Move::parse_uci");
    require(limits.depth == 5, "searchmoves must not consume a following depth field");

    const auto single_move = koi::uci::parse_go_limits("searchmoves a2a3");
    require(single_move.search_moves_specified && single_move.search_moves.size() == 1,
            "searchmoves must keep its restriction flag with one coordinate move");

    const auto no_valid_moves = koi::uci::parse_go_limits("searchmoves depth 5");
    require(no_valid_moves.search_moves_specified && no_valid_moves.search_moves.empty(),
            "searchmoves without a syntactically valid move must keep an empty restriction");
    require(no_valid_moves.depth == 5,
            "searchmoves without a move must leave the following depth field available");

    const auto absent = koi::uci::parse_go_limits("depth 5");
    require(!absent.search_moves_specified && absent.search_moves.empty(),
            "absent searchmoves must leave the restriction clear");

    const koi::SearchInfo info;
    require(info.multipv == 1, "default SearchInfo must use multipv 1");

    const koi::SearchOptions options;
    require(options.multi_pv == 1 && !options.analyse_mode,
            "default SearchOptions must use one principal variation and normal mode");
}

void test_go_limit_parser_uses_fallback_for_missing_malformed_and_overflow_values() {
    const std::vector<std::string_view> commands{
        "",
        "depth nodes movetime wtime btime winc binc movestogo",
        "depth 0 nodes nope movetime -1 wtime bad btime -1 winc nope binc -4 movestogo 0",
        "depth 2147483648 nodes 18446744073709551616 movetime 9223372036854775808 "
        "wtime 9223372036854775808 btime 9223372036854775808 "
        "winc 9223372036854775808 binc 9223372036854775808 movestogo 4294967296",
    };

    for (const std::string_view command : commands) {
        const koi::SearchLimits limits = koi::uci::parse_go_limits(command);

        require(limits.movetime == std::chrono::milliseconds{250},
                "an unusable go command must fall back to the 250 ms movetime");
        require(!limits.nodes.has_value(),
                "invalid node values must not become explicit limits");
        require(!limits.white_clock.has_value() && !limits.black_clock.has_value(),
                "invalid clock values must not create clock limits");
        require(!limits.moves_to_go.has_value() && !limits.infinite,
                "invalid movestogo and absent infinite must remain unset");
    }
}

void test_ponder_option_emits_a_legal_second_pv_move() {
    GatedInputBuffer input(
        "setoption name Ponder value true\n"
        "setoption name OwnBook value false\n"
        "position startpos\n"
        "go depth 2\n",
        "stop\n"
        "quit\n");
    std::istream input_stream(&input);
    ReleaseOnDepthBuffer output_buffer(input, 2);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(output_buffer.str()), "bestmove ");

    require(marker_seen, "Ponder=true must receive a completed two-move PV");
    require(exit_code == 0 && diagnostics.str().empty(), "Ponder=true transcript must shut down normally");
    require(bestmoves.size() == 1, "a completed Ponder-enabled search must emit one result");
    std::istringstream line(bestmoves.front());
    std::string name;
    std::string best;
    std::string ponder_name;
    std::string ponder;
    require(line >> name >> best >> ponder_name >> ponder && name == "bestmove" &&
                ponder_name == "ponder",
            "Ponder=true must append the completed PV reply to bestmove");

    const Position root;
    require(is_legal_move(root, best), "the Ponder-enabled bestmove must be legal at the root");
    const auto parsed_best = koi::Move::parse_uci(best);
    const auto parsed_ponder = koi::Move::parse_uci(ponder);
    require(parsed_best.has_value(), "the Ponder-enabled bestmove must parse");
    require(parsed_ponder.has_value(), "the emitted ponder move must parse");
    koi::GameState after_best = koi::GameState::startpos();
    require(after_best.make_move(*parsed_best) && after_best.is_legal(*parsed_ponder),
            "the emitted ponder move must be legal after the emitted bestmove");
}

void test_invalid_position_commands_preserve_the_previous_position() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "position startpos moves e2e4 not-a-move\n"
        "position fen malformed\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> errors = lines_starting_with(lines, "info string ");
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(errors.size() == 2, "each rejected position command must report one UCI error");
    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove 0000",
            "failed position commands must leave checkmate unchanged");
}

void test_terminal_position_returns_0000() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove 0000",
            "a terminal position must return exactly one bestmove 0000");
}

void test_isready_remains_responsive_during_infinite_search() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go infinite\n"
        "isready\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");
    const std::size_t ready = line_index(lines, "readyok");
    const std::size_t bestmove = bestmoves.empty() ? std::numeric_limits<std::size_t>::max()
                                                   : line_index(lines, bestmoves[0]);

    require(ready < bestmove, "isready must respond before stop completes an infinite search");
    require(bestmoves.size() == 1, "stop must emit exactly one final bestmove");
}

void test_position_replacement_suppresses_the_stale_generation() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go infinite\n"
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go infinite\n"
        "stop\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove 0000",
            "position replacement must suppress the prior generation and duplicate stops");
}

void test_ucinewgame_and_hash_changes_suppress_active_generations() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go infinite\n"
        "ucinewgame\n"
        "go infinite\n"
        "setoption name Hash value 32\n"
        "go infinite\n"
        "setoption name Clear Hash\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    const Position initial;

    require(bestmoves.size() == 1,
            "ucinewgame, Hash, and Clear Hash must suppress each replaced generation");
    require(is_legal_move(initial, first_bestmove_move(bestmoves[0])),
            "the surviving post-option search must use the reset start position");
}

void test_ucinewgame_clears_the_transposition_table_and_records_it() {
    koi::test::TempDirectory files;
    const ControllerResult result = run_controller_in_directory(
        "setoption name Debug value true\n"
        "setoption name DebugFile value ucinewgame-debug.log\n"
        "position startpos\n"
        "go depth 1\n"
        "ucinewgame\n"
        "quit\n", files.path());
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "ucinewgame with hidden diagnostics must leave stdio clean");
    std::ifstream log(files.path() / "ucinewgame-debug.log", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(log)), {});
    require(contents.find("\"event\":\"ucinewgame\"") != std::string::npos &&
                contents.find("\"hash_cleared\":true") != std::string::npos,
            "ucinewgame must clear the transposition table and record the event");
}

void test_debug_command_toggles_hidden_diagnostics() {
    koi::test::TempDirectory files;
    const ControllerResult result = run_controller_in_directory(
        "setoption name DebugFile value debug-command.log\n"
        "debug on\n"
        "position startpos\n"
        "debug off\n"
        "isready\n"
        "quit\n", files.path());
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "the standard debug command must not disturb UCI stdio");
    std::ifstream log(files.path() / "debug-command.log", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(log)), {});
    require(contents.find("command position startpos") != std::string::npos,
            "debug on must enable the hidden diagnostics log");
    require(contents.find("command isready") == std::string::npos,
            "debug off must disable the hidden diagnostics log");
    require(!std::filesystem::exists(files.path() / "koi-debug.log"),
            "the debug command must honor the configured DebugFile path");
}

void test_network_overhead_is_learned_from_clock_discrepancy() {
    koi::test::TempDirectory files;
    const ControllerResult result = run_controller_in_directory(
        "setoption name DebugFile value network-overhead.log\n"
        "debug on\n"
        "position startpos\n"
        "go wtime 1000 btime 1000 winc 0 binc 0\n"
        "go wtime 200 btime 200 winc 0 binc 0\n"
        "quit\n",
        files.path());
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "a clock discrepancy must not disturb UCI stdio");
    std::ifstream log(files.path() / "network-overhead.log", std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(log)), {});
    require(contents.find("network overhead estimate") != std::string::npos,
            "a clock that drops faster than Koi's search time must update the learned overhead");
}
void test_evalfile_autodiscovery_marker_is_informational() {
    const ControllerResult marked = run_controller_with_autodiscovery("uci\nquit\n", true);
    require(marked.exit_code == 0 && marked.diagnostics.empty(),
            "the autodiscovery handshake must stay clean");
    require(lines_starting_with(output_lines(marked.output),
                                "option name EvalFile type string default <autodiscovery>")
                    .size() == 1,
            "a boot-time auto-discovered network must advertise the <autodiscovery> default");

    const ControllerResult plain = run_controller_with_autodiscovery("uci\nquit\n", false);
    require(plain.exit_code == 0 && plain.diagnostics.empty(), "the plain handshake must stay clean");
    require(plain.output.find("<autodiscovery>") == std::string::npos,
            "a non-autodiscovered boot must keep the empty EvalFile default");

    const ControllerResult echoed = run_controller_with_autodiscovery(
        "uci\n"
        "setoption name EvalFile value <autodiscovery>\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n",
        true);
    const std::vector<std::string> lines = output_lines(echoed.output);
    require(echoed.exit_code == 0 && echoed.diagnostics.empty(),
            "echoing the informational marker must stay protocol-clean");
    require(echoed.output.find("EvalFile rejected") == std::string::npos,
            "the informational marker must never be resolved as a path");
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");
    require(bestmoves.size() == 1 && is_legal_move(Position{}, bestmoves.front().substr(9)),
            "search must still return exactly one legal bestmove after echoing the marker");
}
void test_handshake_option_table_is_unique_and_well_formed() {
    const ControllerResult handshake = run_controller("uci\nisready\nquit\n");
    require(handshake.exit_code == 0 && handshake.diagnostics.empty(),
            "a handshake-only transcript must stay clean");
    const std::vector<std::string> options =
        lines_starting_with(output_lines(handshake.output), "option name ");
    require(options.size() == 27, "the handshake must advertise exactly 27 options");

    const std::string prefix = "option name ";
    std::vector<std::string> names;
    std::string defaults_transcript;
    for (const std::string& line : options) {
        const std::size_t type_at = line.find(" type ", prefix.size());
        require(type_at != std::string::npos, "every advertised option must declare a type");
        const std::string name = line.substr(prefix.size(), type_at - prefix.size());
        const std::string rest = line.substr(type_at + std::string(" type ").size());
        require(!name.empty(), "advertised option names must not be empty");
        require(std::find(names.begin(), names.end(), name) == names.end(),
                "advertised option names must be unique: " + name);
        names.push_back(name);

        require(rest == "button" || rest.starts_with("spin ") || rest.starts_with("check ") ||
                    rest.starts_with("string ") || rest.starts_with("combo "),
                "advertised option types must be well formed: " + line);
        if (rest == "button") {
            continue;
        }
        const std::string default_marker = " default ";
        const std::size_t default_at = rest.find(default_marker);
        require(default_at != std::string::npos, "value options must declare a default: " + line);
        std::string default_value = rest.substr(default_at + default_marker.size());
        if (rest.starts_with("spin ")) {
            const std::size_t min_at = default_value.find(" min ");
            require(min_at != std::string::npos, "spin options must declare a minimum: " + line);
            const std::string bounds = default_value.substr(min_at);
            require(bounds.find(" max ") != std::string::npos,
                    "spin options must declare a maximum: " + line);
            default_value = default_value.substr(0, min_at);
        } else if (rest.starts_with("combo ")) {
            require(default_value.find(" var ") != std::string::npos,
                    "combo options must declare their variables: " + line);
            default_value = default_value.substr(0, default_value.find(" var "));
        }
        defaults_transcript += "setoption name " + name + " value " + default_value + "\n";
    }

    const ControllerResult defaults =
        run_controller(defaults_transcript + "isready\nquit\n");
    require(defaults.exit_code == 0 && defaults.diagnostics.empty(),
            "setting every advertised default must leave stdio clean");
    const std::vector<std::string> lines = output_lines(defaults.output);
    require(lines_starting_with(lines, "readyok").size() == 1,
            "setting every advertised default must still answer isready");
    require(lines_starting_with(lines, "bestmove ").empty(),
            "setting options must never start or complete a search");
}

void test_quit_and_eof_join_without_late_bestmove() {
    const ControllerResult quit = run_controller("position startpos\ngo infinite\nquit\n");
    const ControllerResult eof = run_controller("position startpos\ngo infinite\n");

    require(quit.exit_code == 0 && eof.exit_code == 0,
            "quit and EOF must both shut down an active worker cleanly");
    require(lines_starting_with(output_lines(quit.output), "bestmove ").empty(),
            "quit must suppress the active generation's late bestmove");
    require(lines_starting_with(output_lines(eof.output), "bestmove ").empty(),
            "EOF must suppress output while joining the active worker");
}

void test_oversized_protocol_line_is_rejected_and_session_survives() {
    // A command line exactly at the limit is still parsed normally.
    std::string at_limit = "uci";
    at_limit.resize(koi::uci::kMaximumProtocolLineBytes, ' ');
    const ControllerResult accepted = run_controller(at_limit + "\nquit\n");
    require(accepted.exit_code == 0, "a command line at the size limit must be accepted");
    require(lines_starting_with(output_lines(accepted.output), "uciok").size() == 1,
            "a line at the size limit must still parse its command");
    require(accepted.diagnostics.empty(), "an at-limit line must not write stderr");

    // One byte over the limit is consumed and rejected without breaking the
    // session; the diagnostic is a protocol-valid info string.
    std::string oversized(koi::uci::kMaximumProtocolLineBytes + 1, 'x');
    const ControllerResult rejected = run_controller(oversized + "\nisready\nquit\n");
    require(rejected.exit_code == 0, "an oversized line must not terminate the session");
    require(lines_starting_with(output_lines(rejected.output),
                                "info string command rejected: line exceeds").size() == 1,
            "an oversized line must produce exactly one protocol-valid diagnostic");
    require(lines_starting_with(output_lines(rejected.output), "readyok").size() == 1,
            "the session must keep serving after an oversized line");
    require(rejected.diagnostics.empty(), "an oversized line must not write stderr");
}

void test_unknown_stop_and_blank_commands_are_quiet_and_quit() {
    const ControllerResult result = run_controller("\nunknown harmless command\nstop\nquit\n");

    require(result.exit_code == 0, "unknown and idle stop commands must not prevent quit");
    require(result.output.empty(), "blank, unknown, and idle stop commands must be quiet");
}

void test_protocol_output_contains_only_valid_uci_responses() {
    const ControllerResult result = run_controller(
        "uci\n"
        "isready\n"
        "setoption name RandomSeed value 1\n"
        "position startpos\n"
        "go depth 2\n"
        "stop\n"
        "quit\n");

    for (const std::string& line : output_lines(result.output)) {
        const bool valid = line.starts_with("id ") || line.starts_with("option ") ||
            line == "uciok" || line == "readyok" || line.starts_with("bestmove ") ||
            line.starts_with("info string ") || is_valid_search_info(line);
        require(valid, "stdout must contain only valid UCI protocol responses");
    }
    require(result.diagnostics.empty(), "a valid transcript must leave stderr clean");
}

void test_selective_root_score_is_published_unmarked() {
    GatedInputBuffer input(
        "setoption name UCI_ShowWDL value true\n"
        "position fen r1bqk2r/p4ppp/2p2n2/2bpP3/8/2N5/PPP1P1PP/R1BQKB1R b KQkq - 0 8\n"
        "go depth 1\n",
        "stop\nquit\n");
    std::istream input_stream(&input);
    ReleaseOnDepthBuffer output_buffer(input, 1);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool depth_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!depth_seen) {
        input.release();
    }
    controller_thread.join();

    const auto infos = lines_starting_with(output_lines(output_buffer.str()), "info depth 1 ");
    require(depth_seen && exit_code == 0 && diagnostics.str().empty() && !infos.empty(),
            "the selective root fixture must produce a completed UCI depth-one line");
    // GUIs need a score on analysis lines; UCI cannot express "estimate", so
    // the value is published unmarked and never as a bound.
    require(infos.back().find(" score cp ") != std::string::npos ||
                infos.back().find(" score mate ") != std::string::npos,
            "an unproven selective root estimate must still publish its UCI score");
    require(infos.back().find(" wdl ") == std::string::npos &&
                infos.back().find(" lowerbound") == std::string::npos &&
                infos.back().find(" upperbound") == std::string::npos,
            "an unproven selective root estimate must not publish WDL or a bound marker");
}

void test_protocol_responses_flush_promptly() {
    std::istringstream input(
        "uci\n"
        "isready\n"
        "position startpos moves not-a-move\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    FlushTrackingBuffer output_buffer;
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    auto controller_holder = koi::test::make_uci_controller(input, output, diagnostics);
    UciController& controller = *controller_holder;

    require(controller.run() == 0, "flush test transcript must shut down normally");
    require(output_buffer.sync_count() >= 4,
            "handshake, readyok, position error, and bestmove must each flush promptly");
}

// Reads the largest integer that follows a token such as "nodes" or "depth"
// on every info line, so a test can bound what the search actually reported.
std::uint64_t max_reported_value(std::string_view output, std::string_view key) {
    std::uint64_t maximum = 0;
    for (const std::string& line : output_lines(output)) {
        if (!line.starts_with("info depth ")) {
            continue;
        }
        std::istringstream stream(line);
        for (std::string token; stream >> token;) {
            if (token != key) {
                continue;
            }
            std::uint64_t value = 0;
            if (stream >> value) {
                maximum = std::max(maximum, value);
            }
            break;
        }
    }
    return maximum;
}

void test_go_mate_searches_the_proving_depth() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go mate 2\n"
        "stop\n"
        "quit\n");

    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "go mate must shut down cleanly without diagnostics");
    require(bestmoves.size() == 1, "go mate must publish exactly one result");
    require(max_reported_value(result.output, "depth") <= 3,
            "go mate 2 must search the three-ply depth that proves the mate");

    std::istringstream line(bestmoves.front());
    std::string name;
    std::string best;
    require(line >> name >> best && name == "bestmove", "go mate must answer with a bestmove line");
    require(is_legal_move(Position{}, best), "the go mate bestmove must be legal at the root");
}

void test_legacy_strength_options_do_not_cap_clock_searches() {
    koi::test::TempDirectory files;
    const ControllerResult result = run_controller_in_directory(
        "setoption name Debug value true\n"
        "setoption name DebugFile value legacy-strength.log\n"
        "setoption name UCI_LimitStrength value true\n"
        "setoption name UCI_Elo value 1320\n"
        "position startpos\n"
        "go wtime 10000 btime 10000\n"
        "stop\nquit\n", files.path());

    require(result.exit_code == 0 && result.diagnostics.empty(),
            "unknown legacy strength options must leave the controller clean");
    require(lines_starting_with(output_lines(result.output), "bestmove ").size() == 1,
            "unknown legacy strength options must still complete a clock search");
    std::ifstream log_stream(files.path() / "legacy-strength.log");
    const std::string log((std::istreambuf_iterator<char>(log_stream)),
                          std::istreambuf_iterator<char>());
    require(log.find("\"event\":\"go\"") != std::string::npos &&
                log.find("\"limits\":\"depth=-,nodes=-,movetime_ms=-,wtime_ms=10000,btime_ms=10000,") !=
                    std::string::npos,
            "legacy UCI_Elo settings must not add a node cap to a clock search");
}

void test_reapplying_the_current_option_value_keeps_the_search_alive() {
    GatedInputBuffer input(
        "uci\n"
        "setoption name Hash value 1\n"
        "setoption name RandomSeed value 0\n"
        "isready\n"
        "setoption name Threads value 1\n"
        "position startpos\n"
        "go infinite\n",
        "setoption name Threads value 1\n"
        "setoption name Hash value 0001\n"
        "setoption name RandomSeed value 00\n"
        "stop\n"
        "quit\n");
    std::istream input_stream(&input);
    ReleaseOnDepthBuffer output_buffer(input, 1);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();

    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(output_buffer.str()), "bestmove ");
    require(marker_seen && exit_code == 0 && diagnostics.str().empty(),
            "re-applying the current option value must keep the controller quiet");
    require(bestmoves.size() == 1,
            "re-applying the current Threads, Hash, or RandomSeed value must not abort the active search");
}

void test_bestmove_reports_the_ponder_move_without_the_option() {
    GatedInputBuffer input(
        "setoption name OwnBook value false\n"
        "position startpos\n"
        "go depth 2\n",
        "stop\n"
        "quit\n");
    std::istream input_stream(&input);
    ReleaseOnDepthBuffer output_buffer(input, 2);
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        auto controller_holder = koi::test::make_uci_controller(input_stream, output, diagnostics);
        UciController& controller = *controller_holder;
        exit_code = controller.run();
    });
    const bool marker_seen = input.wait_for_marker(std::chrono::seconds(5));
    if (!marker_seen) {
        input.release();
    }
    controller_thread.join();

    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(output_buffer.str()), "bestmove ");
    require(marker_seen && exit_code == 0 && diagnostics.str().empty(),
            "the ponder transcript must shut down normally");
    require(bestmoves.size() == 1, "a completed search must emit one result");

    std::istringstream line(bestmoves.front());
    std::string name;
    std::string best;
    std::string ponder_name;
    std::string ponder;
    require(line >> name >> best >> ponder_name >> ponder && name == "bestmove" &&
                ponder_name == "ponder",
            "bestmove must carry the ponder move even without the Ponder option");

    const auto parsed_best = koi::Move::parse_uci(best);
    const auto parsed_ponder = koi::Move::parse_uci(ponder);
    require(parsed_best.has_value() && parsed_ponder.has_value(),
            "the reported bestmove and ponder move must parse");
    koi::GameState after_best = koi::GameState::startpos();
    require(after_best.make_move(*parsed_best) && after_best.is_legal(*parsed_ponder),
            "the reported ponder move must be legal after the bestmove");
}

void test_uci_annotate_move_and_game_commands() {
    const std::string transcript =
        "uci\n"
        "isready\n"
        "annotate\n"
        "annotate z9z9\n"
        "annotate e2e4 nodes 0\n"
        "annotate game\n"
        "position startpos moves f2f3 e7e5 g2g4 d8h4\n"
        "annotate game depth 3\n"
        "position startpos\n"
        "annotate e2e4 depth 2\n"
        "ucinewgame\n"
        "annotate game\n"
        "quit\n";

    const ControllerResult result = run_controller(transcript);
    require(result.exit_code == 0, "annotate transcript must exit 0");
    require(result.diagnostics.empty(),
            "annotate errors must report via UCI info strings, not stderr");

    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> annotate_lines =
        lines_starting_with(lines, "info string annotate ");
    require(annotate_lines.size() == 12,
            "expected 4 errors + 4 game moves + 2 summaries + 1 single move + 1 post-reset error");

    require(annotate_lines[0].find("annotate error: missing move or game target") !=
                std::string::npos,
            "missing annotate target diagnostic expected");
    require(annotate_lines[1].find("annotate error: invalid move z9z9") != std::string::npos,
            "invalid move diagnostic expected");
    require(annotate_lines[2].find("annotate error: annotation nodes must be positive") !=
                std::string::npos,
            "invalid limit diagnostic expected");
    require(annotate_lines[3].find("annotate error: no moves in current position") !=
                std::string::npos,
            "empty position move history diagnostic expected");

    require(annotate_lines[4].find("annotate move f2f3 ply 0 side white ") != std::string::npos,
            "first ply annotation expected");
    require(annotate_lines[6].find("annotate move g2g4 ply 2 side white ") != std::string::npos &&
                annotate_lines[6].find("classification blunder ") != std::string::npos &&
                annotate_lines[6].find(" glyph ??") != std::string::npos &&
                annotate_lines[6].find(" nag 4") != std::string::npos,
            "g2g4 Fool's Mate blunder line must carry classification, glyph, and NAG");
    require(annotate_lines[7].find("annotate move d8h4 ply 3 side black ") != std::string::npos,
            "fourth ply annotation expected");
    require(annotate_lines[8].find("annotate summary white moves 2 ") != std::string::npos &&
                annotate_lines[8].find(" blunder 1") != std::string::npos,
            "white summary must report 2 moves and 1 blunder");
    require(annotate_lines[9].find("annotate summary black moves 2 ") != std::string::npos,
            "black summary must report 2 moves");
    require(annotate_lines[10].find("annotate move e2e4 ply 0 side white ") != std::string::npos,
            "single-move annotate after position reset expected");
    require(result.output.find("annotate error: no moves in current position",
                               result.output.find("annotate move e2e4")) != std::string::npos,
            "ucinewgame must clear stored game moves before subsequent annotate game");
}

void test_protocol_option_and_handshake_modes() {
    require(koi::UciController::is_valid_protocol("uci") &&
                koi::UciController::is_valid_protocol("usi") &&
                koi::UciController::is_valid_protocol("ucci") &&
                koi::UciController::is_valid_protocol("ucicyclone") &&
                koi::UciController::is_valid_protocol("XBOARD") &&
                !koi::UciController::is_valid_protocol("shogi"),
            "protocol validator must accept all five supported presets and reject unknowns");

    const ControllerResult combo = run_controller(
        "uci\n"
        "setoption name Protocol value nonsense\n"
        "setoption name Protocol value usi\n"
        "usi\n"
        "setoption name Protocol value ucci\n"
        "ucci\n"
        "setoption name Protocol value ucicyclone\n"
        "ucci\n"
        "isready\n"
        "position startpos\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    require(combo.exit_code == 0 && combo.diagnostics.empty(),
            "protocol switches must remain clean");
    require(combo.output.find(
                "option name Protocol type combo default uci var uci var usi var ucci var ucicyclone var xboard") !=
                std::string::npos,
            "UCI handshake must advertise all five Protocol combo presets");
    require(combo.output.find("usiok") != std::string::npos,
            "USI handshake must emit usiok");
    require(combo.output.find("ucciok") != std::string::npos,
            "UCCI handshake must emit ucciok");
    require(combo.output.find("ucicyclone") != std::string::npos,
            "UCI_Cyclone handshake must reflect ucicyclone preset");
    require(combo.output.find("bestmove ") != std::string::npos,
            "UCCI/UCI_Cyclone mode must emit a legal bestmove");
}

void test_usi_and_ucci_commands_in_standard_chess() {
    const ControllerResult usi = run_controller(
        "usi\n"
        "isready\n"
        "usinewgame\n"
        "position sfen 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\n"
        "go byoyomi 50\n"
        "stop\n"
        "quit\n");
    require(usi.exit_code == 0 && usi.diagnostics.empty(),
            "USI compatibility session must exit cleanly");
    require(usi.output.find("usiok") != std::string::npos &&
                usi.output.find("readyok") != std::string::npos &&
                usi.output.find("bestmove resign") != std::string::npos,
            "USI terminal position must report bestmove resign");

    const ControllerResult ucci = run_controller(
        "ucci\n"
        "setoption OwnBook false\n"
        "isready\n"
        "position fen 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\n"
        "go time 50\n"
        "stop\n"
        "bye\n");
    require(ucci.exit_code == 0 && ucci.diagnostics.empty(),
            "UCCI session must support shorthand setoption, go time, nobestmove, and bye");
    require(ucci.output.find("ucciok") != std::string::npos &&
                ucci.output.find("readyok") != std::string::npos &&
                ucci.output.find("nobestmove") != std::string::npos,
            "UCCI terminal position must emit nobestmove");
}

void test_xboard_cecp_protocol_gameplay_and_controls() {
    const ControllerResult xboard = run_controller(
        "xboard\n"
        "protover 2\n"
        "ping 7\n"
        "variant suicide\n"
        "variant normal\n"
        "new\n"
        "option OwnBook=false\n"
        "post\n"
        "sd 1\n"
        "level 0 1:30 2\n"
        "time 9000\n"
        "otim 9000\n"
        "force\n"
        "usermove e2e4\n"
        "usermove e7e5\n"
        "undo\n"
        "usermove c7c5\n"
        "go\n"
        "ping 8\n"
        "usermove e9e9\n"
        "setboard 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\n"
        "go\n"
        "uci\n"
        "quit\n");
    require(xboard.exit_code == 0 && xboard.diagnostics.empty(),
            "XBoard session must remain clean and return exit code 0");
    require(xboard.output.find("feature myname=\"Koi ") != std::string::npos &&
                xboard.output.find("usermove=1") != std::string::npos &&
                xboard.output.find("done=1") != std::string::npos,
            "protover 2 must emit CECP v2 feature string");
    require(xboard.output.find("pong 7") != std::string::npos &&
                xboard.output.find("pong 8") != std::string::npos,
            "ping must return matching pong replies");
    require(xboard.output.find("Error (unknown variant): suicide") != std::string::npos,
            "non-normal variants must be rejected cleanly");
    require(xboard.output.find("move ") != std::string::npos,
            "XBoard go must emit a CECP move reply");
    require(xboard.output.find("Illegal move: e9e9") != std::string::npos,
            "invalid XBoard moves must report Illegal move");
    require(xboard.output.find("1-0 {White mates}") != std::string::npos,
            "mated position in XBoard go must emit mate result");
    require(xboard.output.find("uciok") != std::string::npos,
            "switching back from XBoard via uci must emit uciok");
}

class ThrowingTestEvaluator final : public koi::Evaluator {
public:
    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }

    [[nodiscard]] int evaluate(const koi::GameState&, koi::Color) const override {
        throw std::runtime_error("injected evaluation failure");
    }
};

void test_xboard_cecp_numeric_bounds_and_playother() {
    const ControllerResult numeric = run_controller(
        "xboard\n"
        "protover 2\n"
        "option OwnBook=false\n"
        "st nan\n"
        "st -nan\n"
        "st inf\n"
        "st -inf\n"
        "st 0\n"
        "level 40 5 nan\n"
        "level 40 5 inf\n"
        "level 4294967297 5 0\n"
        "time 9223372036854775807\n"
        "otim -9223372036854775808\n"
        "sd 1\n"
        "go\n"
        "ping 11\n"
        "quit\n");
    const std::vector<std::string> numeric_lines = output_lines(numeric.output);
    const std::vector<std::string> numeric_moves = lines_starting_with(numeric_lines, "move ");
    require(numeric.exit_code == 0 && numeric.diagnostics.empty(),
            "hostile XBoard numeric parameters must stay UB-free and protocol clean");
    require(numeric.output.find("pong 11") != std::string::npos,
            "XBoard session must keep answering ping after out-of-range numeric tokens");
    require(lines_starting_with(numeric_lines, "info string ").empty(),
            "XBoard mode must not leak UCI info string lines");
    require(numeric_moves.size() == 1 && numeric_moves[0].size() > 5 &&
                is_legal_move(Position{}, numeric_moves[0].substr(5)),
            "XBoard go must still emit one legal move after clamped/rejected clocks");

    GatedInputBuffer gated(
        "xboard\n"
        "protover 2\n"
        "option OwnBook=false\n"
        "force\n"
        "usermove e2e4\n"
        "analyze\n",
        "playother\n"
        "ping 42\n"
        "sd 1\n"
        "usermove e7e5\n"
        "ping 43\n"
        "quit\n");
    auto evaluator = std::make_shared<PonderGateEvaluator>(gated);
    std::istream input_stream(&gated);
    std::ostringstream output;
    std::ostringstream diagnostics;
    int exit_code = -1;
    std::thread controller_thread([&] {
        UciController controller(input_stream, output, diagnostics,
                                 koi::SearchService{evaluator});
        exit_code = controller.run();
    });
    require(evaluator->wait_for_started(std::chrono::seconds(5)),
            "XBoard analyze must start before playother arrives");
    evaluator->release();
    controller_thread.join();

    Position after_e4_e5;
    require(after_e4_e5.make_move(*koi::Move::parse_uci("e2e4")) &&
                after_e4_e5.make_move(*koi::Move::parse_uci("e7e5")),
            "e2e4 e7e5 fixture moves must be legal");
    const std::vector<std::string> playother_lines = output_lines(output.str());
    const std::vector<std::string> playother_moves =
        lines_starting_with(playother_lines, "move ");
    require(exit_code == 0 && diagnostics.str().empty(),
            "XBoard playother during active analysis must exit cleanly");
    require(output.str().find("pong 42") != std::string::npos &&
                output.str().find("pong 43") != std::string::npos,
            "playother must stop active analyze so subsequent ping/play proceed");
    require(lines_starting_with(playother_lines, "info string ").empty(),
            "XBoard playother session must not emit UCI info string lines");
    require(playother_moves.size() == 1 && playother_moves[0].size() > 5 &&
                is_legal_move(after_e4_e5, playother_moves[0].substr(5)),
            "after playother clears analyze mode, opponent move must produce one legal CECP move");
}

void test_cross_protocol_switch_stops_active_search_and_preserves_cyclone() {
    auto verify_switch_suppresses = [](std::string_view prefix, std::string_view suffix,
                                       std::string_view expected_ack) {
        GatedInputBuffer gated{std::string(prefix), std::string(suffix)};
        auto evaluator = std::make_shared<PonderGateEvaluator>(gated);
        std::istream input_stream(&gated);
        std::ostringstream output;
        std::ostringstream diagnostics;
        int exit_code = -1;
        std::thread controller_thread([&] {
            UciController controller(input_stream, output, diagnostics,
                                     koi::SearchService{evaluator});
            exit_code = controller.run();
        });
        require(evaluator->wait_for_started(std::chrono::seconds(5)),
                "source protocol search must begin before switching protocol");
        evaluator->release();
        controller_thread.join();

        const std::vector<std::string> lines = output_lines(output.str());
        require(exit_code == 0 && diagnostics.str().empty(),
                "protocol switch during active search must shut down cleanly");
        require(output.str().find(expected_ack) != std::string::npos &&
                    lines_starting_with(lines, "readyok").size() >= 1,
                "protocol switch must acknowledge the target protocol and stay responsive");
        require(lines_starting_with(lines, "bestmove ").empty() &&
                    lines_starting_with(lines, "nobestmove").empty(),
                "switching protocols mid-search must suppress stale move replies");
    };

    verify_switch_suppresses(
        "usi\nisready\nsetoption name OwnBook value false\nposition startpos\ngo infinite\n",
        "uci\nisready\nquit\n", "uciok");
    verify_switch_suppresses(
        "uci\nsetoption name OwnBook value false\nposition startpos\ngo infinite\n",
        "ucci\nsetoption OwnBook false\nisready\nquit\n", "ucciok");

    const ControllerResult cyclone = run_controller(
        "setoption name Protocol value ucicyclone\n"
        "ucci\n"
        "isready\n"
        "position fen 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> cyclone_lines = output_lines(cyclone.output);
    require(cyclone.exit_code == 0 && cyclone.diagnostics.empty(),
            "ucicyclone ucci flow must stay clean");
    require(cyclone.output.find("ucicyclone") != std::string::npos &&
                cyclone.output.find("ucciok") != std::string::npos &&
                lines_starting_with(cyclone_lines, "bestmove ").size() == 1 &&
                cyclone.output.find("bestmove 0000") != std::string::npos &&
                lines_starting_with(cyclone_lines, "nobestmove").empty(),
            "ucci command under ucicyclone preset must preserve ucicyclone bestmove 0000 semantics");
}

void test_xboard_terminal_and_failure_replies_stay_protocol_clean() {
    auto evaluator = std::make_shared<ThrowingTestEvaluator>();

    {
        std::istringstream input(
            "xboard\n"
            "protover 2\n"
            "option OwnBook=false\n"
            "sd 1\n"
            "go\n"
            "ping 9\n"
            "quit\n");
        std::ostringstream output;
        std::ostringstream diagnostics;
        UciController controller(input, output, diagnostics, koi::SearchService{evaluator});
        require(controller.run() == 0 && diagnostics.str().empty(),
                "XBoard failing search must return cleanly without crashing");
        const std::vector<std::string> lines = output_lines(output.str());
        const std::vector<std::string> moves = lines_starting_with(lines, "move ");
        require(lines_starting_with(lines, "info string ").empty() &&
                    lines_starting_with(lines, "bestmove ").empty(),
                "XBoard mode must suppress UCI info string and bestmove lines on search failure");
        require(moves.size() == 1 && moves[0].size() > 5 &&
                    is_legal_move(Position{}, moves[0].substr(5)),
                "XBoard mode must still emit one legal CECP fallback move when evaluation throws");
        require(output.str().find("pong 9") != std::string::npos,
                "XBoard controller must remain responsive after an evaluation failure");
    }

    const ControllerResult stalemate = run_controller(
        "xboard\n"
        "protover 2\n"
        "setboard 7k/5Q2/6K1/8/8/8/8/8 b - - 0 1\n"
        "go\n"
        "ping 10\n"
        "quit\n");
    const std::vector<std::string> stalemate_lines = output_lines(stalemate.output);
    require(stalemate.exit_code == 0 && stalemate.diagnostics.empty() &&
                stalemate.output.find("1/2-1/2 {Stalemate}") != std::string::npos &&
                stalemate.output.find("pong 10") != std::string::npos &&
                lines_starting_with(stalemate_lines, "info string ").empty() &&
                lines_starting_with(stalemate_lines, "bestmove ").empty(),
            "XBoard stalemate must emit CECP result without UCI tokens");

    {
        std::istringstream input(
            "uci\n"
            "setoption name OwnBook value false\n"
            "position startpos\n"
            "go ponder depth 2\n"
            "ponderhit\n"
            "ponderhit\n"
            "stop\n"
            "quit\n");
        std::ostringstream output;
        std::ostringstream diagnostics;
        UciController controller(input, output, diagnostics, koi::SearchService{evaluator});
        require(controller.run() == 0 && diagnostics.str().empty(),
                "failing ponder search followed by duplicate ponderhit must exit cleanly");
        const std::vector<std::string> lines = output_lines(output.str());
        const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");
        require(bestmoves.size() == 1 && is_legal_move(Position{}, first_bestmove_move(bestmoves[0])),
                "failing ponder search must answer exactly one legal bestmove");
    }
}

void test_annotate_rejects_invalid_limits_and_debug_log_stays_nonthrowing() {
    const ControllerResult result = run_controller(
        "uci\n"
        "isready\n"
        "position startpos\n"
        "annotate e2e4 depth 0\n"
        "annotate e2e4 depth\n"
        "annotate e2e4 movetime -1\n"
        "annotate e2e4 infinite\n"
        "annotate e2e4 ponder\n"
        "annotate e2e4 nodes 100\n"
        "quit\n");
    require(result.exit_code == 0 && result.diagnostics.empty(),
            "invalid annotate limit rejection must keep controller clean");
    const std::vector<std::string> lines =
        lines_starting_with(output_lines(result.output), "info string annotate ");
    require(lines.size() == 6,
            "expected five annotate errors followed by one valid default/node-capped annotation");
    require(lines[0].find("annotate error: annotation depth must be positive") != std::string::npos &&
                lines[1].find("annotate error: annotation depth must be positive") !=
                    std::string::npos &&
                lines[2].find("annotate error: annotation movetime must be non-negative") !=
                    std::string::npos &&
                lines[3].find("annotate error: unsupported annotation limit: infinite") !=
                    std::string::npos &&
                lines[4].find("annotate error: unsupported annotation limit: ponder") !=
                    std::string::npos,
            "invalid or unsupported annotate limits must report explicit errors");
    require(lines[5].find("annotate move e2e4 ") != std::string::npos,
            "valid annotate limits after errors must still run");

    koi::test::TempDirectory temp;
    koi::detail::DebugLog log;
    log.set_requested_path("missing_subdir/koi-debug.log");
    log.set_enabled(true);
    log.configure();
    log.write_event("probe", "\"ok\":true");
    log.set_executable_directory(temp.path());
    log.set_requested_path("clean-debug.log");
    log.configure();
    log.write_event("probe", "\"ok\":true");
    require(std::filesystem::exists(temp.path() / "clean-debug.log"),
            "DebugLog must recover and write once configured with a valid directory");
}

void test_xboard_suppresses_uci_info_strings_across_all_handlers() {
    koi::test::TempDirectory temp;
    const std::filesystem::path book = temp.path() / "xboard-book.bin";
    const koi::GameState start = koi::GameState::startpos();
    write_book(book, {
        {start.polyglot_key(), polyglot_move("e2", "e4"), 100, 0},
    });
    const std::filesystem::path syzygy_dir = temp.path() / "syzygy";
    std::filesystem::create_directories(syzygy_dir);
    {
        std::ofstream corrupt(syzygy_dir / "KQvK.rtbw", std::ios::binary);
        corrupt << "not-syzygy";
    }

    const std::string oversized(koi::uci::kMaximumProtocolLineBytes + 32, 'x');
    std::istringstream input(
        "xboard\n"
        "protover 2\n"
        "memory 999999999\n"
        "cores 9999\n"
        "option EvalFile=does-not-exist-xboard.nnue\n"
        "option SyzygyPath=" + syzygy_dir.string() + "\n"
        "option SyzygyPath=\n"
        "option BookFile=" + book.string() + "\n"
        "option OwnBook=true\n"
        "setboard 7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\n"
        "new\n"
        "position malformed_in_xboard\n"
        "perft 1\n"
        "annotate e2e4 depth 1\n" +
        oversized + "\n"
        "sd 1\n"
        "go\n"
        "option OwnBook=false\n"
        "new\n"
        "sd 1\n"
        "go\n"
        "ping 42\n"
        "quit\n");
    std::ostringstream output;
    std::ostringstream diagnostics;
    koi::UciController controller(
        input, output, diagnostics,
        koi::SearchService(std::make_shared<koi::test::MaterialEvaluator>(), {}, 16), temp.path());
    const int exit_code = controller.run();

    const std::string raw = output.str();
    const std::vector<std::string> lines = output_lines(raw);
    const std::vector<std::string> info_lines = lines_starting_with(lines, "info ");
    const std::vector<std::string> bestmove_lines = lines_starting_with(lines, "bestmove ");
    const std::vector<std::string> uciok_lines = lines_starting_with(lines, "uciok");
    const std::vector<std::string> move_lines = lines_starting_with(lines, "move ");

    require(exit_code == 0, "XBoard info-suppression session must exit cleanly with 0");
    require(info_lines.empty() && bestmove_lines.empty() && uciok_lines.empty(),
            "XBoard session must never emit UCI info/bestmove/uciok lines; got:\n" + raw);
    require(raw.find("pong 42") != std::string::npos,
            "XBoard ping must remain responsive after oversized/rejected commands");
    require(move_lines.size() == 2 && move_lines[0] == "move e2e4",
            "XBoard book and search moves must still emit clean CECP move replies");
}

void test_interactive_engine_commands_display_eval_compiler_and_bench() {
    std::istringstream input(
        "position startpos moves e2e4\n"
        "d\n"
        "eval\n"
        "flip\n"
        "d\n"
        "position fen 4k3/8/8/8/8/8/4R3/4K3 b - - 0 1\n"
        "flip\n"
        "compiler\n"
        "help\n"
        "bench 2 16 1\n"
        "quit\n");
    std::ostringstream output;
    std::ostringstream diagnostics;
    koi::UciController controller(
        input, output, diagnostics,
        koi::SearchService(std::make_shared<koi::test::MaterialEvaluator>(), {}, 16));
    const int exit_code = controller.run();

    const std::string raw = output.str();
    require(exit_code == 0, "interactive engine commands must exit cleanly with 0");
    require(raw.find("Fen   : rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1") !=
                std::string::npos,
            "d command must print the current position FEN; got:\n" + raw);
    require(raw.find("Key   : ") != std::string::npos &&
                raw.find("Side  : black") != std::string::npos &&
                raw.find("Moves : 20") != std::string::npos,
            "d command must print Zobrist key, side to move, and legal move count");
    require(raw.find("Side to move   :") != std::string::npos &&
                raw.find("White relative :") != std::string::npos &&
                raw.find("WDL (stm)      :") != std::string::npos &&
                raw.find("Game phase     :") != std::string::npos,
            "eval command must print score, WDL triplet, and game phase");
    require(raw.find("flipped side to move (white)") != std::string::npos &&
                raw.find("Fen   : rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 1") !=
                    std::string::npos,
            "flip command must switch side to move and update the board; got:\n" + raw);
    require(raw.find("flip rejected: position cannot legally switch sides") != std::string::npos,
            "flip must reject positions that would leave the non-moving king in check");
    require(raw.find("Compiled by    :") != std::string::npos &&
                raw.find("SIMD / ISA     :") != std::string::npos,
            "compiler command must print toolchain and ISA metadata");
    require(raw.find("Koi interactive commands:") != std::string::npos &&
                raw.find("see <move>") != std::string::npos,
            "help command must list interactive commands");
    require(raw.find("Total time (ms) :") != std::string::npos &&
                raw.find("Nodes searched  :") != std::string::npos &&
                raw.find("Nodes/second    :") != std::string::npos,
            "bench command must print OpenBench-compatible node and NPS summary");

    GatedInputBuffer perft_input("position startpos\nperft 1\n", "quit\n");
    std::istream perft_in(&perft_input);
    ReleaseOnSubstringBuffer perft_out(perft_input, "Nodes searched: 20");
    std::ostream perft_out_stream(&perft_out);
    std::ostringstream perft_diag;
    int perft_exit = -1;
    std::thread perft_thread([&] {
        auto perft_holder =
            koi::test::make_uci_controller(perft_in, perft_out_stream, perft_diag);
        perft_exit = perft_holder->run();
    });
    const bool saw_perft = perft_input.wait_for_marker(std::chrono::seconds(5));
    perft_input.release();
    perft_thread.join();
    require(saw_perft && perft_exit == 0 && perft_diag.str().empty(),
            "bare perft 1 command must compute 20 startpos nodes and shut down cleanly");

    std::istringstream wait_input(
        "uci\n"
        "isready\n"
        "position startpos\n"
        "perft 1\n"
        "wait\n"
        "go depth 2\n"
        "wait\n"
        "go infinite\n"
        "wait\n"
        "stop\n"
        "quit\n");
    std::ostringstream wait_output;
    std::ostringstream wait_diag;
    auto wait_holder = koi::test::make_uci_controller(wait_input, wait_output, wait_diag);
    const int wait_exit = wait_holder->run();
    const std::string wait_raw = wait_output.str();
    require(wait_exit == 0 && wait_diag.str().empty(),
            "wait command sequence must shut down cleanly");
    require(wait_raw.find("Nodes searched: 20") != std::string::npos,
            "wait after perft must await completion before starting the next command");
    require(wait_raw.find("wait ignored: active search has no finite limit") != std::string::npos,
            "wait during an infinite search must refuse cleanly without hanging");
    std::size_t wait_bestmove_count = 0;
    for (std::size_t pos = 0;
         (pos = wait_raw.find("\nbestmove ", pos)) != std::string::npos;
         pos += 10) {
        ++wait_bestmove_count;
    }
    require(wait_bestmove_count == 2,
            "wait after finite search plus stop after infinite search must emit two bestmoves; got:\n" +
                wait_raw);

    std::istringstream see_input(
        "position fen 4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1\n"
        "see e4d5\n"
        "see e1e2\n"
        "see e1e8\n"
        "quit\n");
    std::ostringstream see_output;
    std::ostringstream see_diag;
    auto see_holder = koi::test::make_uci_controller(see_input, see_output, see_diag);
    const int see_exit = see_holder->run();
    const std::string see_raw = see_output.str();
    require(see_exit == 0 && see_diag.str().empty(),
            "see command sequence must shut down cleanly");
    require(see_raw.find("SEE e4d5 : +100 cp") != std::string::npos,
            "see must report capturing a pawn as +100 cp; got:\n" + see_raw);
    require(see_raw.find("SEE e1e2 : +0 cp") != std::string::npos,
            "see must report a quiet move as +0 cp; got:\n" + see_raw);
    require(see_raw.find("see rejected illegal move 'e1e8'") != std::string::npos,
            "see must reject illegal moves cleanly; got:\n" + see_raw);

    std::istringstream nnue_eval_in("position startpos\nfen\nkey\nmoves\nbook\ntb\neval\nposition fen 8/8/8/8/8/6K1/5Q2/7k b - - 0 1\nmoves\nquit\n");
    std::ostringstream nnue_eval_out;
    std::ostringstream nnue_eval_diag;
    koi::UciController nnue_eval_ctrl(
        nnue_eval_in, nnue_eval_out, nnue_eval_diag,
        koi::SearchService(
            std::make_shared<koi::NnueEvaluator>(
                std::make_shared<koi::NnueNetwork>(koi::test::synthetic_network_v4())),
            {}, 16));
    const int nnue_eval_exit = nnue_eval_ctrl.run();
    const std::string nnue_eval_raw = nnue_eval_out.str();
    require(nnue_eval_exit == 0 &&
                nnue_eval_raw.find("Evaluator      : NNUE (v4 halfka-king-bucket-v1, ") !=
                    std::string::npos,
            "eval with NnueEvaluator must report NNUE version, feature set, and dimensions; got:\n" +
                nnue_eval_raw);
    require(nnue_eval_raw.find("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\n") !=
                std::string::npos,
            "fen must print exact six-field FEN; got:\n" + nnue_eval_raw);
    require(nnue_eval_raw.find("Position key : ") != std::string::npos &&
                nnue_eval_raw.find("Pawn key     : ") != std::string::npos &&
                nnue_eval_raw.find("Polyglot key : ") != std::string::npos,
            "key must print hex Zobrist position, pawn, and Polyglot keys; got:\n" + nnue_eval_raw);
    require(nnue_eval_raw.find("Legal moves (20): a2a3 ") != std::string::npos &&
                nnue_eval_raw.find("Legal moves (0): (none)\n") != std::string::npos,
            "moves must print sorted legal moves count and (none) at checkmate; got:\n" +
                nnue_eval_raw);
    require(nnue_eval_raw.find("Opening book : unavailable\n") != std::string::npos &&
                nnue_eval_raw.find("Syzygy : unavailable\n") != std::string::npos,
            "book and tb must report unavailable when unconfigured; got:\n" + nnue_eval_raw);

    const koi::test::TempDirectory book_dir;
    const std::filesystem::path interactive_book = book_dir.path() / "interactive.bin";
    const koi::GameState startpos = koi::GameState::startpos();
    write_book(interactive_book, {
        {startpos.polyglot_key(), polyglot_move("e2", "e4"), 75, 0},
        {startpos.polyglot_key(), polyglot_move("d2", "d4"), 25, 0},
    });
    std::istringstream book_cmd_in(
        "setoption name BookFile value " + interactive_book.string() +
        "\nposition startpos\nbook\nquit\n");
    std::ostringstream book_cmd_out;
    std::ostringstream book_cmd_diag;
    auto book_cmd_ctrl = koi::test::make_uci_controller(book_cmd_in, book_cmd_out, book_cmd_diag);
    const int book_cmd_exit = book_cmd_ctrl->run();
    const std::string book_cmd_raw = book_cmd_out.str();
    require(book_cmd_exit == 0 && book_cmd_diag.str().empty(),
            "book inspection sequence must shut down cleanly");
    require(book_cmd_raw.find("Opening book : interactive.bin (2 moves, total weight 100)") !=
                std::string::npos &&
                book_cmd_raw.find("  e2e4  weight=75 (75.0%)") != std::string::npos &&
                book_cmd_raw.find("  d2d4  weight=25 (25.0%)") != std::string::npos,
            "book command must print sorted weighted entries; got:\n" + book_cmd_raw);

    std::istringstream cli_eval_in;
    std::ostringstream cli_eval_out;
    std::ostringstream cli_eval_diag;
    auto cli_eval_ctrl = koi::test::make_uci_controller(cli_eval_in, cli_eval_out, cli_eval_diag);
    require(cli_eval_ctrl->run_eval("4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1") == 0 &&
                cli_eval_out.str().find("Evaluator      : ") != std::string::npos,
            "run_eval with custom FEN must evaluate cleanly; got:\n" + cli_eval_out.str());
    require(cli_eval_ctrl->run_eval("not-a-valid-fen") == 2 &&
                cli_eval_diag.str().find("invalid FEN") != std::string::npos,
            "run_eval with malformed FEN must return exit code 2 and diagnostic");
}

void test_uci_support_helpers_direct() {
    using namespace koi::detail;

    const std::optional<std::string> flipped =
        flip_position_fen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e3 0 1");
    require(flipped.has_value() &&
                *flipped == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
            "flip_position_fen must toggle side and clear en passant square");
    require(!flip_position_fen("8/8/8/8 w - -").has_value() &&
                !flip_position_fen("8/8/8/8/8/8/8/8 x - - 0 1").has_value(),
            "flip_position_fen must reject malformed FENs");

    const std::optional<koi::Move> e2e4 = koi::Move::parse_uci("e2e4");
    const std::optional<koi::Move> d2d4 = koi::Move::parse_uci("d2d4");
    require(e2e4.has_value() && d2d4.has_value(), "expected valid UCI moves");

    koi::MoveAnnotationResult move_ann;
    move_ann.ply = 4;
    move_ann.side_to_move = koi::Color::black;
    move_ann.played_move = *d2d4;
    move_ann.best_move = *e2e4;
    move_ann.annotation = koi::MoveAnnotation::best;
    move_ann.centipawn_loss = 0;
    move_ann.accuracy_percent = 99.5;
    move_ann.win_prob_before = 0.520;
    move_ann.win_prob_after = 0.515;
    move_ann.played_eval = {.score_cp = 15};
    move_ann.best_eval = {.score_cp = 15};
    move_ann.depth = 12;
    move_ann.best_pv = {*e2e4};
    const std::string ann_line = format_annotate_move_line(move_ann);
    require(ann_line.rfind("annotate move d2d4 ply 4 side black best e2e4", 0) == 0 &&
                ann_line.find("info string") == std::string::npos &&
                ann_line.find(" pv e2e4") != std::string::npos,
            "format_annotate_move_line must be protocol-literal-free; got: " + ann_line);

    koi::PlayerAnnotationSummary summary;
    summary.move_count = 10;
    summary.best = 6;
    summary.inaccuracy = 1;
    const std::string sum_line = format_annotate_summary_line("white", summary);
    require(sum_line.rfind("annotate summary white moves 10", 0) == 0 &&
                sum_line.find("info string") == std::string::npos,
            "format_annotate_summary_line must be protocol-literal-free; got: " + sum_line);

    const std::string empty_book = format_book_entries("book.bin", {});
    const std::string filled_book = format_book_entries(
        "book.bin", {{*e2e4, 75, 0}, {*d2d4, 25, 0}});
    require(empty_book.find("no moves for position") != std::string::npos &&
                filled_book.find("book.bin (2 moves, total weight 100)") != std::string::npos &&
                filled_book.find("e2e4  weight=75 (75.0%)") != std::string::npos,
            "format_book_entries mismatch");

    const koi::GameState startpos = koi::GameState::startpos();
    require(format_syzygy_probe(nullptr, startpos) == "Syzygy : unavailable\n",
            "format_syzygy_probe(nullptr) must report unavailable");

    const std::string key_info = format_key_info(startpos);
    require(key_info.find("Position key : ") != std::string::npos &&
                key_info.find("Pawn key     : ") != std::string::npos &&
                key_info.find("Polyglot key : ") != std::string::npos,
            "format_key_info missing hash keys");

    const auto mate_pos =
        koi::GameState::from_fen("8/8/8/8/8/6K1/5Q2/7k b - - 0 1");
    require(mate_pos.has_value() &&
                format_legal_moves(startpos).find("Legal moves (20):") == 0 &&
                format_legal_moves(*mate_pos) == "Legal moves (0): (none)\n",
            "format_legal_moves mismatch");

    require(translate_ucci_go_arguments("depth 4 time 1500 opptime 1200 increment 50 oppincrement 25",
                                        koi::Color::white) ==
                "depth 4 wtime 1500 btime 1200 winc 50 binc 25",
            "translate_ucci_go_arguments white mapping mismatch");
    require(translate_ucci_go_arguments("time 1500 opptime 1200 increment 50 oppincrement 25",
                                        koi::Color::black) ==
                "btime 1500 wtime 1200 binc 50 winc 25",
            "translate_ucci_go_arguments black mapping mismatch");
    require(translate_usi_go_arguments("byoyomi 500") == "movetime 500" &&
                translate_usi_go_arguments("depth 6 byoyomi 500") == "depth 6",
            "translate_usi_go_arguments mapping mismatch");

    const koi::ClassicalEvaluator hce_eval;
    const std::string hce_summary = format_eval_summary(startpos, &hce_eval, true);
    require(hce_summary.find("HCE (classical tapered)") != std::string::npos &&
                hce_summary.find("Term           |") != std::string::npos &&
                hce_summary.find("Material") != std::string::npos &&
                hce_summary.find("Tempo") != std::string::npos &&
                hce_summary.find("Total (White)") != std::string::npos,
            "format_eval_summary with ClassicalEvaluator must emit HCE breakdown table; got:\n" +
                hce_summary);

    const std::string help_text = format_interactive_help();
    for (const char* cmd : {"bench", "d", "fen", "key", "moves", "book", "tb", "eval", "flip",
                            "see", "perft", "wait", "compiler", "help"}) {
        require(help_text.find(cmd) != std::string::npos,
                std::string("format_interactive_help missing command: ") + cmd);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"uci handshake and options", test_uci_handshake_has_identity_and_supported_options_in_order},
        {"EvalMode during search and rejected EvalFile", test_eval_mode_switch_during_search_and_rejected_evalfile},
        {"EvalFile restores NNUE after HCE", test_evalfile_load_after_hce_reports_nnue_mode},
        {"EvalMode evaluator selection", test_eval_mode_switches_between_hce_and_the_boot_evaluator},
        {"Syzygy option values", test_syzygy_options_accept_valid_values_and_ignore_invalid_values},
        {"legacy strength handshake", test_legacy_strength_options_are_not_advertised},
        {"legacy strength unknown options", test_legacy_strength_options_are_quiet_unknown_options},
        {"surviving UCI strength options",
         test_surviving_uci_strength_options_accept_valid_and_ignore_invalid_values},
        {"En Croissant option casing", test_en_croissant_option_names_are_case_insensitive},
        {"Task 1 WDL", test_task1_wdl_output_is_optional_and_mate_scores_are_converted},
        {"Task 1 hidden diagnostics", test_task1_hidden_debug_file_is_relative_rotated_and_off_stdio},
        {"public option replacement", test_public_option_change_emits_exactly_one_bestmove},
        {"Task 6 StrengthMode", test_task6_strength_mode_is_case_insensitive_and_cancels_active_search},
        {"opening-book normal play", test_book_options_emit_one_seeded_marker_and_bestmove_for_normal_play},
        {"opening-book auto detection", test_opening_book_is_auto_detected},
        {"invalid book is not announced", test_invalid_book_is_not_announced_as_detected},
        {"opening-book random option", test_book_random_option_accepts_valid_values_and_ignores_invalid_values},
        {"opening-book safety option", test_book_safety_options_reject_poisoned_moves_and_fallback_to_search},
        {"opening-book fallback and bypass", test_book_fallback_and_analysis_style_commands_search_without_markers},
        {"opening-book MultiPV bypass", test_multipv_search_bypasses_a_matching_book_without_analysis_mode},
        {"ponderhit book bypass", test_ponderhit_keeps_the_entire_ponder_workflow_out_of_the_book},
        {"opening-book depth boundaries", test_book_depth_boundaries_and_invalid_values_preserve_the_previous_limit},
        {"opening-book option generation replacement", test_book_option_changes_suppress_active_search_generations},
        {"Hash and Clear Hash contract", test_hash_options_preserve_the_contract_and_never_advertise_threads},
        {"large Hash safety", test_large_hash_option_is_capped_without_crashing_the_controller},
        {"hash allocation failure", test_hash_allocation_failure_preserves_table_and_controller},
        {"Threads validation", test_threads_option_accepts_valid_values_and_ignores_invalid_values},
        {"Threads and Move Overhead generation replacement",
         test_threads_and_move_overhead_changes_suppress_the_active_generation},
        {"Lucas analysis options and multipv output",
         test_lucas_analysis_options_accept_valid_values_ignore_invalid_values_and_emit_multipv},
        {"Lucas analysis option generation replacement",
         test_lucas_analysis_option_changes_suppress_the_active_generation},
        {"ponderhit continuation lifecycle", test_ponderhit_continues_the_ponder_search_in_place},
        {"immediate ponderhit completion", test_immediate_ponderhit_without_observed_reply_still_answers},
        {"ponderhit shutdown suppression", test_quit_and_eof_suppress_a_ponderhit_replacement_search},
        {"idle ponderhit", test_idle_ponderhit_is_quiet},
        {"ponder stop lifecycle", test_stopping_ponder_search_emits_one_legal_bestmove},
        {"ponder root filtering", test_stopping_ponder_search_preserves_the_searchmoves_root_filter},
        {"ponder terminal result", test_stopping_terminal_ponder_search_emits_0000},
        {"ready response", test_isready_writes_readyok},
        {"deterministic search", test_deterministic_search_repeats_the_best_move_with_compatibility_seed},
        {"position startpos and FEN", test_startpos_and_fen_move_lists_define_the_search_root},
        {"go limits and malformed values", test_all_go_limits_and_malformed_values_are_accepted_without_crashing},
        {"go limit parser exact mapping", test_go_limit_parser_maps_each_supported_limit_exactly},
        {"go mate and depth limits", test_go_mate_depth_combines_with_explicit_depth},
        {"go limit fallback and clocks", test_go_limit_parser_uses_a_scaled_bare_go_fallback_and_independent_clocks},
        {"go limit parser Lucas root options", test_go_limit_parser_supports_lucas_root_options_and_value_defaults},
        {"go limit parser fallback", test_go_limit_parser_uses_fallback_for_missing_malformed_and_overflow_values},
        {"Ponder PV emission", test_ponder_option_emits_a_legal_second_pv_move},
        {"transactional invalid positions", test_invalid_position_commands_preserve_the_previous_position},
        {"terminal 0000", test_terminal_position_returns_0000},
        {"ready during search", test_isready_remains_responsive_during_infinite_search},
        {"position generation replacement", test_position_replacement_suppresses_the_stale_generation},
        {"new game and hash generation replacement", test_ucinewgame_and_hash_changes_suppress_active_generations},
        {"ucinewgame clears the hash", test_ucinewgame_clears_the_transposition_table_and_records_it},
        {"debug command toggle", test_debug_command_toggles_hidden_diagnostics},
        {"network overhead learning", test_network_overhead_is_learned_from_clock_discrepancy},
        {"handshake option table", test_handshake_option_table_is_unique_and_well_formed},
        {"EvalFile autodiscovery marker", test_evalfile_autodiscovery_marker_is_informational},
        {"quit and EOF cleanup", test_quit_and_eof_join_without_late_bestmove},
        {"bounded protocol line", test_oversized_protocol_line_is_rejected_and_session_survives},
        {"unknown stop quit", test_unknown_stop_and_blank_commands_are_quiet_and_quit},
        {"protocol-clean output", test_protocol_output_contains_only_valid_uci_responses},
        {"selective root score provenance", test_selective_root_score_is_published_unmarked},
        {"promptly flushed responses", test_protocol_responses_flush_promptly},
        {"go mate mapping", test_go_mate_searches_the_proving_depth},
        {"legacy strength node cap", test_legacy_strength_options_do_not_cap_clock_searches},
        {"same-value option reapplication", test_reapplying_the_current_option_value_keeps_the_search_alive},
        {"ponder move without the option", test_bestmove_reports_the_ponder_move_without_the_option},
        {"network overhead estimator", test_network_overhead_estimator_tracks_gui_latency},
        {"uci annotate commands", test_uci_annotate_move_and_game_commands},
        {"Protocol presets and handshakes", test_protocol_option_and_handshake_modes},
        {"USI and UCCI compatibility commands", test_usi_and_ucci_commands_in_standard_chess},
        {"XBoard CECP v2 protocol commands", test_xboard_cecp_protocol_gameplay_and_controls},
        {"XBoard numeric bounds and playother", test_xboard_cecp_numeric_bounds_and_playother},
        {"cross-protocol switch lifecycle",
         test_cross_protocol_switch_stops_active_search_and_preserves_cyclone},
        {"XBoard terminal and failure replies",
         test_xboard_terminal_and_failure_replies_stay_protocol_clean},
        {"annotate limit validation and debug log safety",
         test_annotate_rejects_invalid_limits_and_debug_log_stays_nonthrowing},
        {"XBoard UCI info string suppression",
         test_xboard_suppresses_uci_info_strings_across_all_handlers},
        {"interactive d eval compiler and bench commands",
         test_interactive_engine_commands_display_eval_compiler_and_bench},
        {"detail/uci_support direct helpers", test_uci_support_helpers_direct},
    };

    return koi::test::run_tests(tests, argc, argv);
}
