#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/completion_gate.hpp"
#include "koi/opening_book.hpp"
#include "koi/search_service.hpp"

namespace koi {

namespace detail {

class DebugLog;

} // namespace detail

class SyzygyTablebase;

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
    enum class ProtocolMode : std::uint8_t {
        uci,
        usi,
        ucci,
        ucicyclone,
        xboard,
    };

    UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics,
                  SearchService search_service,
                  std::filesystem::path executable_directory = {},
                  bool nnue_autodiscovered = false,
                  std::filesystem::path boot_nnue_path = {});
    ~UciController();

    UciController(const UciController&) = delete;
    UciController& operator=(const UciController&) = delete;

    static constexpr int kDefaultBenchDepth = 10;
    static constexpr std::size_t kDefaultBenchHashMb = 16;
    static constexpr std::size_t kDefaultBenchThreads = 1;
    static constexpr std::size_t kDefaultSpeedtestThreads = 1;
    static constexpr std::size_t kDefaultSpeedtestHashMb = 128;
    static constexpr int kDefaultSpeedtestSeconds = 3;

    // Selects the initial wire protocol (`uci`, `usi`, `ucci`, `ucicyclone`, or
    // `xboard`).  Returns false when `name` is unrecognized.
    bool set_protocol(std::string_view name);
    void set_initial_hash_mb(std::size_t hash_mb);
    void set_initial_threads(std::size_t threads) noexcept;
    void set_initial_book_path(std::string_view path);
    void set_initial_syzygy_path(std::string_view path);
    [[nodiscard]] static bool is_valid_protocol(std::string_view name) noexcept;

    int run();
    int run_bench(int depth = kDefaultBenchDepth,
                  std::size_t hash_mb = kDefaultBenchHashMb,
                  std::size_t threads = kDefaultBenchThreads);
    int run_speedtest(std::size_t threads = kDefaultSpeedtestThreads,
                      std::size_t hash_mb = kDefaultSpeedtestHashMb,
                      int runtime_seconds = kDefaultSpeedtestSeconds);
    int run_eval(std::optional<std::string_view> fen = std::nullopt);
    int run_options();

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
    void handle_annotate(std::istream& command);
    void handle_display();
    void handle_fen();
    void handle_epd();
    void handle_key();
    void handle_moves(std::istream& command);
    void handle_book();
    void handle_tb();
    void handle_eval();
    void handle_nnue();
    void handle_see(std::istream& command);
    void handle_flip();
    void handle_makemove(std::istream& command);
    void handle_undo(std::istream& command);
    void handle_history();
    void handle_pgn();
    void handle_perft_command(std::istream& command);
    void handle_compiler();
    void handle_bench(std::istream& command);
    void handle_speedtest(std::istream& command);
    void handle_options();
    void handle_wait();
    void handle_help();
    void reset_new_game();
    bool handle_xboard_command(std::string_view name, std::istream& command, bool& should_quit);
    void start_xboard_search(bool analyze_mode);
    void apply_pending_xboard_move();
    void start_search(GameState root, SearchLimits limits, bool skip_book = false,
                      bool force_analyze = false,
                      std::chrono::steady_clock::time_point search_started = {});
    [[nodiscard]] bool debug_logging_enabled() const noexcept;
    bool try_install_nnue_file(const std::filesystem::path& path, std::string_view option_label,
                               std::filesystem::path* resolved_out = nullptr);
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
    void write_handshake(ProtocolMode mode = ProtocolMode::uci);
    void write_xboard_features();
    void write_readyok();
    void write_search_info(std::uint64_t generation, const SearchInfo& info,
                           ProtocolMode protocol, bool xboard_thinking);
    void write_search_completion(std::uint64_t generation, const SearchResult& result,
                                 ProtocolMode protocol, bool is_ponder_search,
                                 bool xboard_analyze, const GameState& search_root);
    void write_book_completion(std::uint64_t generation, const GameState& root,
                               const SearchLimits& limits, const BookChoice& choice,
                               std::uint32_t root_ply,
                               const std::shared_ptr<CompletionOnce>& completion_once,
                               ProtocolMode protocol);
    void write_uci_info_string_locked(std::string_view message);
    void write_uci_info_string(std::string_view message);
    void write_position_error(const char* message);
    // Book auto-detection: a book beside the executable is used without the GUI
    // setting OwnBook, so shipping one is enough for it to take effect.
    [[nodiscard]] std::filesystem::path resolved_book_path() const;
    [[nodiscard]] bool book_is_available() const;
    void write_perft_results(GameState root, int depth, std::stop_token stop_token,
                             std::uint64_t generation);
    void debug_event(std::string message) noexcept;
    void debug_json_event(std::string event, std::string fields) noexcept;
    void rebuild_syzygy(bool emit_diagnostics = true);
    void ensure_auto_syzygy_loaded();

    std::istream& input_;
    std::ostream& output_;
    std::ostream& diagnostics_;
    std::mutex output_mutex_;
    std::filesystem::path executable_directory_;
    GameState position_;
    GameState position_origin_;
    std::vector<Move> position_moves_;

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
    bool active_search_unbounded_ = false;
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
    bool analyse_mode_ = false;
    std::unique_ptr<detail::DebugLog> debug_log_;
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
    // Automatic table discovery: a `syzygy/` directory beside the executable is
    // loaded lazily at the first isready/go unless the GUI sets SyzygyPath.
    bool syzygy_path_explicit_ = false;
    bool syzygy_auto_detected_ = false;
    bool syzygy_auto_loaded_ = false;
    bool syzygy_configured_ = false;
    std::filesystem::path eval_file_;
    std::filesystem::path boot_nnue_path_;
    std::shared_ptr<const Evaluator> boot_evaluator_;
    bool hce_active_ = false;
    // Boot-time autodiscovery actually installed a network: advertise that in
    // the EvalFile handshake default.  Explicit KOI_NNUE_PATH/EvalFile are not
    // autodiscovery.
    bool nnue_autodiscovered_ = false;
    std::uint8_t syzygy_probe_depth_ = 1;
    std::uint8_t syzygy_probe_limit_ = 7;
    std::uint8_t syzygy_interior_depth_ = 0;
    bool syzygy_50_move_rule_ = true;
    std::shared_ptr<SyzygyTablebase> syzygy_;
    CompletionGate completion_gate_;
    // Protocol staleness counter: begin_generation() bumps this for every new
    // search request and write_search_completion() drops replies whose
    // captured generation no longer matches.  This is not the transposition
    // table's replacement epoch (advanced by TranspositionTable::new_generation)
    // and not a SearchRequestIdentity; see search_types.hpp for the three
    // distinct "generation" concepts.
    std::uint64_t generation_ = 0;
    std::string last_command_;
    ProtocolMode protocol_mode_ = ProtocolMode::uci;
    bool xboard_force_ = false;
    bool xboard_post_ = false;
    bool xboard_analyze_ = false;
    bool xboard_ponder_ = false;
    bool xboard_clock_configured_ = false;
    std::optional<int> xboard_moves_to_go_;
    std::chrono::milliseconds xboard_base_time_{300'000};
    std::chrono::milliseconds xboard_increment_{0};
    std::chrono::milliseconds xboard_white_time_{300'000};
    std::chrono::milliseconds xboard_black_time_{300'000};
    std::optional<std::chrono::milliseconds> xboard_fixed_movetime_;
    std::optional<int> xboard_fixed_depth_;
    // Set by the worker inside output_mutex_ when emitting an XBoard `move`
    // and consumed lazily on the main protocol thread so `position_` stays
    // exclusively single-thread owned (no TSan data race).
    std::uint64_t xboard_pending_move_generation_ = 0;
    std::optional<Move> xboard_pending_move_;
    ControllerState state_ = ControllerState::Idle;
};

} // namespace koi
