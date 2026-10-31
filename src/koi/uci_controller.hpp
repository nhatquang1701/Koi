#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "koi/game_state.hpp"
#include "koi/completion_gate.hpp"

#include "koi/opening_book.hpp"
#include "koi/search_service.hpp"
#include "koi/syzygy_tablebase.hpp"

namespace koi {

namespace uci {

// Parses the portion of a UCI "go" command following the command name.
[[nodiscard]] SearchLimits parse_go_limits(std::string_view arguments);

// Estimates how much wall clock a move lost outside search: the GUI/network
// round trip.  `previous_remaining_ms` and `increment_ms` describe the clock the
// GUI reported for the previous move, `elapsed_ms` is Koi's own search time, and
// `current_remaining_ms` is the clock reported for this move.  Positive excess
// above the noise floor raises the estimate; a zero/negative excess decays it
// so a one-off stall does not permanently slow the engine.
[[nodiscard]] std::uint32_t derive_network_overhead_ms(
    std::int64_t previous_remaining_ms, std::int64_t increment_ms,
    std::int64_t elapsed_ms, std::int64_t current_remaining_ms,
    std::uint32_t previous_estimate_ms) noexcept;

// Upper bound on one protocol command line.  Valid UCI commands are far
// smaller (a full-game `position ... moves ...` line is a few kilobytes); the
// cap stops a hostile or broken GUI from forcing an unbounded allocation in the
// controller's line reader.  Longer lines are consumed and rejected with an
// `info string` diagnostic while the session keeps serving.
inline constexpr std::size_t kMaximumProtocolLineBytes = std::size_t{1} << 20;

} // namespace uci

class UciController {
public:
    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics);
    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics,
                  SearchService search_service,
                  std::filesystem::path executable_directory = {},
                  bool nnue_autodiscovered = false);
    ~UciController();

    UciController(const UciController&) = delete;
    UciController& operator=(const UciController&) = delete;

    int run();

private:
    enum class ControllerState : std::uint8_t {
        Idle,
        Searching,
        Pondering,
        Stopping,
        ShuttingDown,
    };

    void handle_position(std::istream& command, std::string_view command_text);
    void handle_setoption(std::istream& command);
    void handle_go(std::istream& command);
    void handle_ponderhit();
    void start_search(GameState root, SearchLimits limits, bool skip_book = false);
    // Records the clock baseline for the previous/current timed move and folds
    // the observed GUI round trip into `observed_gui_overhead_ms_`.
    void note_clock_baseline(const SearchLimits& limits) noexcept;
    [[nodiscard]] std::uint32_t effective_move_overhead_ms() const noexcept;
    void stop_active_search();
    void stop_and_suppress_active_search();
    void start_perft(int depth);
    void stop_active_perft();
    void clear_ponder_state();
    [[nodiscard]] std::uint64_t begin_generation();
    void write_handshake();
    void write_readyok();
    void write_search_info(std::uint64_t generation, const SearchInfo& info);
    void write_search_completion(std::uint64_t generation, const SearchResult& result);
    void write_book_completion(std::uint64_t generation, const GameState& root,
                               const SearchLimits& limits, const BookChoice& choice,
                               std::uint32_t root_ply,
                               const std::shared_ptr<CompletionOnce>& completion_once);
    void write_position_error(const char* message);
    // Book auto-detection: a book beside the executable is used without the GUI
    // setting OwnBook, so shipping one is enough for it to take effect.
    [[nodiscard]] std::filesystem::path resolved_book_path() const;
    [[nodiscard]] bool book_is_available() const;
    void write_perft_results(GameState root, int depth, std::stop_token stop_token,
                             std::uint64_t generation);
    void debug_event(std::string message) noexcept;
    void debug_json_event(std::string event, std::string fields) noexcept;
    void configure_debug_file();
    void rebuild_syzygy();
    [[nodiscard]] std::filesystem::path debug_path() const;
    void rotate_debug_file_if_needed(std::size_t incoming_bytes);

    std::istream& input_;
    std::ostream& output_;
    std::ostream& diagnostics_;
    std::mutex output_mutex_;
    std::mutex debug_mutex_;
    std::filesystem::path executable_directory_;
    std::filesystem::path debug_file_path_;
    std::ofstream debug_file_;
    GameState position_;

    OpeningBook opening_book_;
    SearchService search_service_;
    std::optional<SearchHandle> active_search_;
    std::jthread active_perft_;
    std::atomic_bool perft_running_{false};
    std::optional<GameState> ponder_origin_;
    std::optional<GameState> ponder_root_;
    std::optional<SearchLimits> ponder_limits_;
    std::optional<Move> principal_variation_best_move_;
    std::optional<Move> principal_variation_ponder_move_;
    std::optional<Move> ponder_predicted_move_;
    std::optional<Move> ponder_expected_move_;
    bool active_ponder_ = false;
    std::size_t hash_mb_ = 512;
    std::size_t threads_ = 1;
    bool show_wdl_ = false;
    std::uint32_t move_overhead_ms_ = 30;
    bool dynamic_overhead_ = false;
    // GUI/network overhead learned from clock discrepancies (clock searches
    // only; never from movetime).  Added to the configured Move Overhead.
    std::uint32_t observed_gui_overhead_ms_ = 0;
    bool clock_baseline_valid_ = false;
    std::int64_t previous_clock_remaining_ms_ = -1;
    std::int64_t previous_clock_increment_ms_ = 0;
    std::chrono::steady_clock::time_point previous_clock_search_start_{};
    std::uint32_t slow_mover_percent_ = 100;
    std::uint32_t contempt_cp_ = 0;
    bool strength_mode_ = false;
    bool debug_enabled_ = false;
    bool analyse_mode_ = false;
    std::size_t multi_pv_ = 1;
    bool ponder_enabled_ = false;
    bool own_book_ = false;
    // An opening book shipped beside the executable is used automatically unless
    // the GUI sets OwnBook explicitly, so "install the book" is all a user does.
    bool own_book_explicit_ = false;
    bool book_auto_reported_ = false;
    std::string book_file_ = "book.bin";
    bool book_random_ = false;
    bool book_safety_ = true;
    std::uint8_t book_safety_depth_ = 2;
    std::uint8_t book_depth_ = 16;
    std::uint32_t random_seed_ = 0;
    std::filesystem::path syzygy_path_;
    std::filesystem::path eval_file_;
    // Boot-time autodiscovery actually installed a network: advertise that in
    // the EvalFile handshake default.  Explicit KOI_NNUE_PATH/EvalFile are not
    // autodiscovery.
    bool nnue_autodiscovered_ = false;
    std::uint8_t syzygy_probe_depth_ = 1;
    std::uint8_t syzygy_probe_limit_ = 7;
    std::uint8_t syzygy_interior_depth_ = 0;
    bool syzygy_50_move_rule_ = true;
    std::shared_ptr<SyzygyTablebase> syzygy_ =
        std::make_shared<SyzygyTablebase>();
    CompletionGate completion_gate_;
    // Protocol staleness counter: begin_generation() bumps this for every new
    // search request and write_search_completion() drops replies whose
    // captured generation no longer matches.  This is not the transposition
    // table's replacement epoch (advanced by TranspositionTable::new_generation)
    // and not a SearchRequestIdentity; see search_types.hpp for the three
    // distinct "generation" concepts.
    std::uint64_t generation_ = 0;
    std::string last_command_;
    ControllerState state_ = ControllerState::Idle;
};

} // namespace koi
