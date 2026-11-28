#include "koi/uci_controller.hpp"

#include "koi/detail/debug_log.hpp"
#include "koi/detail/static_exchange.hpp"
#include "koi/detail/uci_option_table.hpp"
#include "koi/detail/uci_support.hpp"
#include "koi/syzygy_tablebase.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/move_annotation.hpp"
#include "koi/nnue.hpp"
#include "koi/perft.hpp"
#include "koi/time_manager.hpp"

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "unknown"
#endif
#ifndef KOI_ENGINE_BUILD_COMMIT
#define KOI_ENGINE_BUILD_COMMIT "unknown"
#endif

namespace koi {

namespace {

using detail::UciOptionDescriptor;
using detail::UciOptionId;
using detail::UciOptionKind;
using detail::BoundedLineRead;
using detail::ParsedOption;
using detail::Wdl;
using detail::cecp_mate_score;
using detail::debug_quoted;
using detail::equals_ignore_case;
using detail::find_uci_option;
using detail::flip_position_fen;
using detail::format_annotate_move_line;
using detail::format_annotate_summary_line;
using detail::hash_reason_name;
using detail::hash_status_name;
using detail::is_bare_coordinate_move_token;
using detail::join_tokens;
using detail::kBenchFens;
using detail::kMaximumHashMegabytes;
using detail::kMaximumMoveOverheadMs;
using detail::kUciOptions;
using detail::legal_move_digest;
using detail::legal_pv_prefix;
using detail::parse_annotate_limits;
using detail::parse_boolean;
using detail::parse_random_seed;
using detail::parse_setoption_for_protocol;
using detail::parse_uint64;
using detail::parse_xboard_centiseconds_ms;
using detail::parse_xboard_level_time;
using detail::parse_xboard_option_assignment;
using detail::parse_xboard_seconds;
using detail::read_bounded_protocol_line;
using detail::remaining_tokens;
using detail::root_ply;
using detail::score_to_wdl;
using detail::translate_ucci_go_arguments;
using detail::translate_usi_go_arguments;
using detail::xboard_terminal_result_line;

// The UCI surface no longer exposes a Speed option; internal time management
// keeps the neutral 100 % factor so the time-manager semantics and the
// koi-bench `--speed` API stay unchanged.
constexpr std::uint8_t kFixedSpeedPercent = 100;
// Bounded fallback for a bare `go` and for malformed or asymmetric clock
// commands. A UCI `go` always has to be answered, so a missing usable limit
// must never leave the engine searching indefinitely.
constexpr std::chrono::milliseconds kBareGoFallback{250};

std::uint64_t option_maximum(const UciOptionDescriptor& option) {
    return option.dynamic_maximum ? static_cast<std::uint64_t>(maximum_search_threads())
                                  : option.maximum;
}

bool parse_bounded_uint64(const std::string_view token, const std::uint64_t min_value,
                          const std::uint64_t max_value, std::uint64_t& out) noexcept {
    return parse_uint64(token, out) && out >= min_value && out <= max_value;
}

std::string display_filename(const std::filesystem::path& path) {
    return path.filename().empty() ? path.string() : path.filename().string();
}

std::optional<UciController::ProtocolMode> parse_protocol_mode(std::string_view value) noexcept {
    if (equals_ignore_case(value, "uci")) {
        return UciController::ProtocolMode::uci;
    }
    if (equals_ignore_case(value, "usi")) {
        return UciController::ProtocolMode::usi;
    }
    if (equals_ignore_case(value, "ucci")) {
        return UciController::ProtocolMode::ucci;
    }
    if (equals_ignore_case(value, "ucicyclone")) {
        return UciController::ProtocolMode::ucicyclone;
    }
    if (equals_ignore_case(value, "xboard")) {
        return UciController::ProtocolMode::xboard;
    }
    return std::nullopt;
}

std::string_view protocol_mode_name(UciController::ProtocolMode mode) noexcept {
    switch (mode) {
    case UciController::ProtocolMode::uci:
        return "uci";
    case UciController::ProtocolMode::usi:
        return "usi";
    case UciController::ProtocolMode::ucci:
        return "ucci";
    case UciController::ProtocolMode::ucicyclone:
        return "ucicyclone";
    case UciController::ProtocolMode::xboard:
        return "xboard";
    }
    return "uci";
}

// Emits the terminal/no-move reply matching the active wire protocol.
// Caller must hold output_mutex_.
void write_no_move_reply(std::ostream& output, const UciController::ProtocolMode protocol,
                         const GameState& root) {
    switch (protocol) {
    case UciController::ProtocolMode::xboard:
        output << xboard_terminal_result_line(root) << '\n' << std::flush;
        return;
    case UciController::ProtocolMode::usi:
        output << "bestmove resign\n" << std::flush;
        return;
    case UciController::ProtocolMode::ucci:
        output << "nobestmove\n" << std::flush;
        return;
    case UciController::ProtocolMode::uci:
    case UciController::ProtocolMode::ucicyclone:
        output << "bestmove 0000\n" << std::flush;
        return;
    }
}

// Translates USI/UCCI/UCI_Cyclone `go` limit extensions cleanly onto Koi's
// standard SearchLimits without altering ordinary UCI parsing.
SearchLimits parse_protocol_go_limits(const UciController::ProtocolMode mode,
                                      std::string_view arguments,
                                      const Color side_to_move) {
    switch (mode) {
    case UciController::ProtocolMode::ucci:
    case UciController::ProtocolMode::ucicyclone:
        return uci::parse_go_limits(translate_ucci_go_arguments(arguments, side_to_move));
    case UciController::ProtocolMode::usi:
        return uci::parse_go_limits(translate_usi_go_arguments(arguments));
    case UciController::ProtocolMode::uci:
    case UciController::ProtocolMode::xboard:
        return uci::parse_go_limits(arguments);
    }
    return uci::parse_go_limits(arguments);
}

} // namespace

namespace uci {

SearchLimits parse_go_limits(std::string_view arguments) {
    return detail::parse_go_limits(arguments, kBareGoFallback);
}

std::uint32_t derive_network_overhead_ms(std::int64_t previous_remaining_ms,
                                         std::int64_t increment_ms,
                                         std::int64_t elapsed_ms,
                                         std::int64_t current_remaining_ms,
                                         std::uint32_t previous_estimate_ms) noexcept {
    return detail::derive_network_overhead_ms(previous_remaining_ms, increment_ms, elapsed_ms,
                                              current_remaining_ms, previous_estimate_ms);
}

} // namespace uci

UciController::UciController(std::istream& input, std::ostream& output,
                             std::ostream& diagnostics, SearchService search_service,
                             std::filesystem::path executable_directory,
                             bool nnue_autodiscovered,
                             std::filesystem::path boot_nnue_path)
    : input_(input), output_(output), diagnostics_(diagnostics),
      executable_directory_(std::move(executable_directory)),
      opening_book_(executable_directory_),
      search_service_(std::move(search_service)),
      nnue_autodiscovered_(nnue_autodiscovered),
      boot_nnue_path_(std::move(boot_nnue_path)) {
    boot_evaluator_ = search_service_.evaluator();
    hce_active_ = dynamic_cast<const ClassicalEvaluator*>(boot_evaluator_.get()) != nullptr;
    debug_log_ = std::make_unique<detail::DebugLog>();
    debug_log_->set_executable_directory(executable_directory_);
    syzygy_ = std::make_shared<SyzygyTablebase>();
    if (!executable_directory_.empty()) {
        if (const auto discovered = discover_syzygy_directory(executable_directory_);
            discovered.has_value()) {
            syzygy_path_ = *discovered;
            syzygy_auto_detected_ = true;
        }
    }
}

UciController::~UciController() {
    {
        std::lock_guard lock(output_mutex_);
        state_ = ControllerState::ShuttingDown;
    }
    stop_and_suppress_active_search();
}

bool UciController::is_valid_protocol(const std::string_view name) noexcept {
    return parse_protocol_mode(name).has_value();
}

bool UciController::set_protocol(const std::string_view name) {
    const auto parsed = parse_protocol_mode(name);
    if (!parsed.has_value()) {
        return false;
    }
    stop_and_suppress_active_search();
    std::lock_guard lock(output_mutex_);
    protocol_mode_ = *parsed;
    xboard_force_ = false;
    xboard_analyze_ = false;
    xboard_pending_move_.reset();
    return true;
}

void UciController::set_initial_hash_mb(const std::size_t hash_mb) {
    const std::size_t clamped =
        std::clamp<std::size_t>(hash_mb, 1, static_cast<std::size_t>(kMaximumHashMegabytes));
    const HashResizeResult result = search_service_.set_hash_size_mb(clamped);
    hash_mb_ = result.effective_mb;
}

void UciController::set_initial_threads(const std::size_t threads) noexcept {
    threads_ = std::clamp<std::size_t>(threads, 1, maximum_search_threads());
}

void UciController::set_initial_book_path(const std::string_view path) {
    if (path.empty()) {
        return;
    }
    std::filesystem::path resolved(path);
    if (resolved.is_relative()) {
        std::error_code ec;
        if (std::filesystem::exists(resolved, ec) && !ec) {
            const std::filesystem::path abs = std::filesystem::absolute(resolved, ec);
            if (!ec) {
                resolved = abs;
            }
        }
    }
    own_book_explicit_ = true;
    own_book_ = true;
    book_file_ = resolved.string();
    opening_book_.set_file(book_file_);
}

void UciController::set_initial_syzygy_path(const std::string_view path) {
    if (path.empty()) {
        return;
    }
    syzygy_path_explicit_ = true;
    syzygy_auto_detected_ = false;
    const bool path_list = path.find(kSyzygyPathSeparator) != std::string_view::npos;
    std::filesystem::path resolved(path);
    if (!path_list && resolved.is_relative()) {
        std::error_code ec;
        if (std::filesystem::exists(resolved, ec) && !ec) {
            const std::filesystem::path abs = std::filesystem::absolute(resolved, ec);
            if (!ec) {
                resolved = abs;
            }
        } else if (!executable_directory_.empty()) {
            resolved = executable_directory_ / resolved;
        }
    }
    syzygy_path_ = std::move(resolved);
    rebuild_syzygy(false);
}

void UciController::reset_new_game() {
    stop_and_suppress_active_search();
    search_service_.clear_hash();
    {
        std::lock_guard lock(output_mutex_);
        observed_gui_overhead_ms_ = 0;
        clock_baseline_valid_ = false;
        previous_clock_search_completed_ = false;
        active_clock_search_generation_ = 0;
        previous_clock_remaining_ms_ = -1;
        previous_clock_increment_ms_ = 0;
        previous_clock_search_elapsed_ms_ = 0;
        previous_clock_search_start_ = {};
        previous_clock_side_ = Color::white;
        previous_clock_fullmove_ = 0;
        previous_clock_position_key_ = 0;
        previous_search_score_.reset();
        previous_search_side_ = Color::white;
        previous_search_fullmove_ = 0;
        previous_search_position_key_ = 0;
        xboard_pending_move_.reset();
    }
    debug_json_event("ucinewgame", "\"hash_cleared\":true");
    position_ = GameState::startpos();
    position_origin_ = GameState::startpos();
    position_moves_.clear();
}

void UciController::apply_pending_xboard_move() {
    std::optional<Move> pending;
    {
        std::lock_guard lock(output_mutex_);
        if (xboard_pending_move_.has_value() &&
            xboard_pending_move_generation_ == generation_) {
            pending = xboard_pending_move_;
        }
        xboard_pending_move_.reset();
    }
    if (pending.has_value() && position_.make_move(*pending)) {
        position_moves_.push_back(*pending);
    }
}

void UciController::start_xboard_search(const bool analyze_mode) {
    const auto search_started = std::chrono::steady_clock::now();
    ensure_auto_syzygy_loaded();
    stop_and_suppress_active_search();
    apply_pending_xboard_move();
    if (!analyze_mode && !position_.has_legal_move()) {
        std::lock_guard lock(output_mutex_);
        output_ << xboard_terminal_result_line(position_) << '\n' << std::flush;
        return;
    }
    SearchLimits limits;
    if (analyze_mode) {
        limits.infinite = true;
    } else {
        if (xboard_fixed_depth_.has_value()) {
            limits.depth = *xboard_fixed_depth_;
        }
        if (xboard_fixed_movetime_.has_value()) {
            limits.movetime = *xboard_fixed_movetime_;
        } else if (xboard_clock_configured_) {
            limits.white_clock = ClockLimit{xboard_white_time_, xboard_increment_};
            limits.black_clock = ClockLimit{xboard_black_time_, xboard_increment_};
            if (xboard_moves_to_go_.has_value() && *xboard_moves_to_go_ > 0) {
                limits.moves_to_go = static_cast<std::uint32_t>(*xboard_moves_to_go_);
            }
        } else if (!limits.depth.has_value()) {
            limits.movetime = std::chrono::milliseconds{1'000};
        }
    }
    start_search(position_, std::move(limits), analyze_mode, analyze_mode, search_started);
}

bool UciController::handle_xboard_command(const std::string_view name, std::istream& command,
                                          bool& should_quit) {
    if (name == "xboard") {
        stop_and_suppress_active_search();
        std::lock_guard lock(output_mutex_);
        protocol_mode_ = ProtocolMode::xboard;
        xboard_force_ = false;
        xboard_analyze_ = false;
        xboard_pending_move_.reset();
        return true;
    }
    if (name == "protover") {
        int version = 0;
        (void)(command >> version);
        write_xboard_features();
        return true;
    }
    if (name == "accepted" || name == "rejected" || name == "random" ||
        name == "computer" || name == "name" || name == "rating" ||
        name == "ics" || name == "hint" || name == "bk" || name == "draw" ||
        name == "result" || name == "white" || name == "black" || name == "option") {
        if (name == "option") {
            const std::vector<std::string> tokens = remaining_tokens(command);
            if (!tokens.empty()) {
                if (const auto parsed_opt =
                        parse_xboard_option_assignment(join_tokens(tokens, 0, tokens.size()));
                    parsed_opt.has_value()) {
                    std::istringstream synth("name " + parsed_opt->first + " value " +
                                             parsed_opt->second);
                    handle_setoption(synth);
                }
            }
        }
        return true;
    }
    if (name == "new") {
        reset_new_game();
        xboard_force_ = false;
        xboard_analyze_ = false;
        xboard_clock_configured_ = false;
        xboard_fixed_movetime_.reset();
        xboard_fixed_depth_.reset();
        xboard_white_time_ = std::chrono::milliseconds{300'000};
        xboard_black_time_ = std::chrono::milliseconds{300'000};
        xboard_increment_ = std::chrono::milliseconds{0};
        xboard_moves_to_go_.reset();
        return true;
    }
    if (name == "variant") {
        std::string variant_name;
        if ((command >> variant_name) && !equals_ignore_case(variant_name, "normal") &&
            !equals_ignore_case(variant_name, "standard")) {
            std::lock_guard lock(output_mutex_);
            output_ << "Error (unknown variant): " << variant_name << '\n' << std::flush;
        }
        return true;
    }
    if (name == "wait") {
        handle_wait();
        return true;
    }
    if (name == "quit") {
        {
            std::lock_guard lock(output_mutex_);
            state_ = ControllerState::ShuttingDown;
        }
        stop_and_suppress_active_search();
        should_quit = true;
        return true;
    }
    if (name == "force") {
        stop_and_suppress_active_search();
        apply_pending_xboard_move();
        xboard_force_ = true;
        xboard_analyze_ = false;
        return true;
    }
    if (name == "go") {
        xboard_force_ = false;
        xboard_analyze_ = false;
        start_xboard_search(false);
        return true;
    }
    if (name == "playother") {
        stop_and_suppress_active_search();
        apply_pending_xboard_move();
        xboard_force_ = false;
        xboard_analyze_ = false;
        return true;
    }
    if (name == "post") {
        std::lock_guard lock(output_mutex_);
        xboard_post_ = true;
        return true;
    }
    if (name == "nopost") {
        std::lock_guard lock(output_mutex_);
        xboard_post_ = false;
        return true;
    }
    if (name == "hard") {
        stop_and_suppress_active_search();
        xboard_ponder_ = true;
        ponder_enabled_ = true;
        return true;
    }
    if (name == "easy") {
        stop_and_suppress_active_search();
        xboard_ponder_ = false;
        ponder_enabled_ = false;
        return true;
    }
    if (name == "?") {
        stop_active_search();
        return true;
    }
    if (name == "ping") {
        std::string token;
        if (command >> token) {
            bool can_wait = false;
            {
                std::lock_guard lock(output_mutex_);
                can_wait = active_search_.has_value() && !xboard_analyze_ && !active_ponder_;
            }
            if (can_wait && active_search_.has_value()) {
                active_search_->wait();
                active_search_.reset();
                apply_pending_xboard_move();
            }
            ensure_auto_syzygy_loaded();
            std::lock_guard lock(output_mutex_);
            output_ << "pong " << token << '\n' << std::flush;
        }
        return true;
    }
    if (name == "memory") {
        std::string token;
        if (command >> token) {
            std::istringstream synth("name Hash value " + token);
            handle_setoption(synth);
        }
        return true;
    }
    if (name == "cores") {
        std::string token;
        if (command >> token) {
            std::istringstream synth("name Threads value " + token);
            handle_setoption(synth);
        }
        return true;
    }
    if (name == "setboard") {
        stop_and_suppress_active_search();
        const std::vector<std::string> tokens = remaining_tokens(command);
        if (tokens.size() < 4) {
            std::lock_guard lock(output_mutex_);
            output_ << "tellusererror Illegal position\n" << std::flush;
            return true;
        }
        std::string fen = join_tokens(tokens, 0, tokens.size());
        if (tokens.size() == 4) {
            fen += " 0 1";
        }
        const auto parsed = GameState::from_fen(fen);
        if (!parsed) {
            std::lock_guard lock(output_mutex_);
            output_ << "tellusererror Illegal position: " << fen << '\n' << std::flush;
            return true;
        }
        position_origin_ = *parsed;
        position_ = *parsed;
        position_moves_.clear();
        {
            std::lock_guard lock(output_mutex_);
            xboard_pending_move_.reset();
        }
        if (xboard_analyze_) {
            start_xboard_search(true);
        }
        return true;
    }
    if (name == "level") {
        std::string mps_tok, base_tok, inc_tok;
        if (command >> mps_tok >> base_tok >> inc_tok) {
            std::uint64_t mps = 0;
            std::chrono::milliseconds base{};
            std::chrono::milliseconds inc{};
            if (parse_uint64(mps_tok, mps) && parse_xboard_level_time(base_tok, base) &&
                parse_xboard_seconds(inc_tok, inc)) {
                constexpr std::uint64_t kMaxMovesToGo =
                    static_cast<std::uint64_t>(std::numeric_limits<int>::max());
                xboard_moves_to_go_ = mps > 0
                    ? std::optional<int>(static_cast<int>(std::min(mps, kMaxMovesToGo)))
                    : std::nullopt;
                xboard_base_time_ = base;
                xboard_increment_ = inc;
                xboard_white_time_ = base;
                xboard_black_time_ = base;
                xboard_fixed_movetime_.reset();
                xboard_clock_configured_ = true;
            }
        }
        return true;
    }
    if (name == "st") {
        std::string sec_tok;
        std::chrono::milliseconds parsed{};
        if ((command >> sec_tok) && parse_xboard_seconds(sec_tok, parsed) &&
            parsed.count() > 0) {
            xboard_fixed_movetime_ = parsed;
            xboard_clock_configured_ = false;
        }
        return true;
    }
    if (name == "sd") {
        std::string depth_tok;
        std::uint64_t depth = 0;
        if ((command >> depth_tok) && parse_uint64(depth_tok, depth) && depth > 0 &&
            depth <= 64) {
            xboard_fixed_depth_ = static_cast<int>(depth);
        }
        return true;
    }
    if (name == "time" || name == "otim") {
        std::string cs_tok;
        if (command >> cs_tok) {
            if (const auto ms = parse_xboard_centiseconds_ms(cs_tok); ms.has_value()) {
                const bool engine_is_white = (position_.side_to_move() == Color::white);
                const bool target_white = (name == "time") ? engine_is_white : !engine_is_white;
                if (target_white) {
                    xboard_white_time_ = *ms;
                } else {
                    xboard_black_time_ = *ms;
                }
                xboard_clock_configured_ = true;
            }
        }
        return true;
    }
    if (name == "analyze") {
        xboard_analyze_ = true;
        xboard_force_ = false;
        start_xboard_search(true);
        return true;
    }
    if (name == "exit") {
        if (xboard_analyze_) {
            stop_and_suppress_active_search();
            xboard_analyze_ = false;
        }
        return true;
    }
    if (name == "undo" || name == "remove") {
        stop_and_suppress_active_search();
        apply_pending_xboard_move();
        const std::size_t pop_count = (name == "remove") ? 2U : 1U;
        if (position_moves_.size() >= pop_count) {
            rewind_position_history(pop_count);
        }
        if (xboard_analyze_) {
            start_xboard_search(true);
        }
        return true;
    }

    std::string move_token;
    if (name == "usermove") {
        if (!(command >> move_token)) {
            return true;
        }
    } else if (is_bare_coordinate_move_token(name)) {
        move_token = std::string(name);
    } else {
        return false;
    }

    stop_and_suppress_active_search();
    apply_pending_xboard_move();
    const std::optional<Move> parsed_move = detail::parse_move_uci_or_san(position_, move_token);
    if (!parsed_move.has_value() || !position_.is_legal(*parsed_move) ||
        !position_.make_move(*parsed_move)) {
        std::lock_guard lock(output_mutex_);
        output_ << "Illegal move: " << move_token << '\n' << std::flush;
        return true;
    }
    position_moves_.push_back(*parsed_move);
    if (xboard_analyze_) {
        start_xboard_search(true);
    } else if (!xboard_force_) {
        start_xboard_search(false);
    }
    return true;
}

int UciController::run() {
    std::string line;
    for (;;) {
        bool oversized = false;
        if (read_bounded_protocol_line(input_, line, uci::kMaximumProtocolLineBytes, oversized) ==
            BoundedLineRead::eof) {
            break;
        }
        if (oversized) {
            // A hostile or broken GUI must not force an unbounded allocation;
            // report a protocol-valid diagnostic and keep serving.
            write_uci_info_string("command rejected: line exceeds " +
                                  std::to_string(uci::kMaximumProtocolLineBytes) + " bytes");
            debug_event("command rejected: oversized line");
            continue;
        }
        // A GUI on Linux may send CRLF line endings; strip the carriage return
        // so the last token of the command parses like it does on Windows.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        {
            std::lock_guard lock(output_mutex_);
            last_command_ = line;
        }
        if (debug_logging_enabled()) {
            debug_event("command " + line);
        }
        std::istringstream command(line);
        std::string name;
        if (!(command >> name)) {
            continue;
        }

        try {
        ProtocolMode current_protocol = ProtocolMode::uci;
        {
            std::lock_guard lock(output_mutex_);
            current_protocol = protocol_mode_;
        }
        const auto switch_to_uci_family = [&](const ProtocolMode target,
                                              const bool preserve_cyclone) {
            const bool mode_changed = preserve_cyclone
                ? (current_protocol != ProtocolMode::ucci &&
                   current_protocol != ProtocolMode::ucicyclone)
                : (current_protocol != target);
            if (mode_changed) {
                stop_and_suppress_active_search();
            }
            ProtocolMode next_mode = target;
            {
                std::lock_guard lock(output_mutex_);
                if (preserve_cyclone && protocol_mode_ == ProtocolMode::ucicyclone) {
                    next_mode = ProtocolMode::ucicyclone;
                } else {
                    protocol_mode_ = target;
                }
                xboard_force_ = false;
                xboard_analyze_ = false;
                xboard_pending_move_.reset();
            }
            write_handshake(next_mode);
        };
        if (name == "uci") {
            switch_to_uci_family(ProtocolMode::uci, false);
        } else if (name == "usi") {
            switch_to_uci_family(ProtocolMode::usi, false);
        } else if (name == "ucci") {
            switch_to_uci_family(ProtocolMode::ucci, true);
        } else if (name == "xboard" || current_protocol == ProtocolMode::xboard) {
            bool should_quit = false;
            if (handle_xboard_command(name, command, should_quit)) {
                if (should_quit) {
                    return 0;
                }
            }
        } else if (name == "isready") {
            write_readyok();
        } else if (name == "ucinewgame" ||
                   (current_protocol == ProtocolMode::usi && name == "usinewgame") ||
                   ((current_protocol == ProtocolMode::ucci ||
                     current_protocol == ProtocolMode::ucicyclone) &&
                    name == "uccinewgame")) {
            reset_new_game();
        } else if (name == "position") {
            handle_position(command, line);
        } else if (name == "setoption") {
            handle_setoption(command);
        } else if (name == "go") {
            handle_go(command);
        } else if (name == "annotate") {
            handle_annotate(command);
        } else if (name == "d") {
            handle_display();
        } else if (name == "fen") {
            handle_fen();
        } else if (name == "epd") {
            handle_epd();
        } else if (name == "key") {
            handle_key();
        } else if (name == "moves") {
            handle_moves(command);
        } else if (name == "book") {
            handle_book();
        } else if (name == "tb") {
            handle_tb();
        } else if (name == "eval") {
            handle_eval();
        } else if (name == "nnue") {
            handle_nnue();
        } else if (name == "see") {
            handle_see(command);
        } else if (name == "flip") {
            handle_flip();
        } else if (name == "makemove") {
            handle_makemove(command);
        } else if (name == "undo") {
            handle_undo(command);
        } else if (name == "history") {
            handle_history();
        } else if (name == "pgn") {
            handle_pgn();
        } else if (name == "perft") {
            handle_perft_command(command);
        } else if (name == "compiler") {
            handle_compiler();
        } else if (name == "bench") {
            handle_bench(command);
        } else if (name == "speedtest") {
            handle_speedtest(command);
        } else if (name == "options") {
            handle_options();
        } else if (name == "wait") {
            handle_wait();
        } else if (name == "help") {
            handle_help();
        } else if (name == "ponderhit") {
            handle_ponderhit();
        } else if (name == "debug") {
            // Standard UCI `debug on|off` command. It aliases the unadvertised
            // Debug option so both spellings configure the same diagnostics.
            std::string debug_value;
            if ((command >> debug_value) &&
                (equals_ignore_case(debug_value, "on") ||
                 equals_ignore_case(debug_value, "off"))) {
                const bool debug = equals_ignore_case(debug_value, "on");
                if (debug_log_->enabled() != debug) {
                    stop_and_suppress_active_search();
                    debug_log_->set_enabled(debug);
                    debug_log_->configure();
                }
            }
        } else if (name == "stop") {
            stop_active_search();
        } else if (name == "quit" ||
                   ((current_protocol == ProtocolMode::ucci ||
                     current_protocol == ProtocolMode::ucicyclone) &&
                    name == "bye")) {
            {
                std::lock_guard lock(output_mutex_);
                state_ = ControllerState::ShuttingDown;
            }
            stop_and_suppress_active_search();
            return 0;
        } else if (current_protocol == ProtocolMode::usi && name == "gameover") {
            stop_and_suppress_active_search();
        }
        } catch (const std::exception& error) {
            // A malformed or resource-starved command must not terminate the
            // engine mid-game: report it as a protocol-valid info line and keep
            // serving.  quit/EOF cancellation semantics are unchanged.
            write_uci_info_string(std::string("command failed: ") + error.what());
        } catch (...) {
            write_uci_info_string("command failed");
        }
    }

    stop_and_suppress_active_search();
    return 0;
}

void UciController::handle_position(std::istream& command, std::string_view command_text) {
    // Suppress the old generation before doing any parsing work. A worker may
    // finish while a replacement position is being validated; it must not be
    // allowed to emit a completion for the old root during that window. The
    // suppression bumps the generation and leaves the state Idle, so a malformed
    // position command no longer latches the controller in ShuttingDown.
    stop_and_suppress_active_search();
    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.empty()) {
        write_position_error("missing position");
        return;
    }

    GameState candidate = GameState::startpos();
    std::size_t next = 0;
    ProtocolMode current_protocol = ProtocolMode::uci;
    {
        std::lock_guard lock(output_mutex_);
        current_protocol = protocol_mode_;
    }
    if (tokens[0] == "startpos") {
        next = 1;
    } else if (tokens[0] == "fen" ||
               (current_protocol == ProtocolMode::usi && tokens[0] == "sfen")) {
        if (tokens.size() < 7) {
            write_position_error("invalid FEN");
            return;
        }

        const std::string fen = join_tokens(tokens, 1, 7);
        const auto parsed = GameState::from_fen(fen);
        if (!parsed) {
            write_position_error("invalid FEN");
            return;
        }
        candidate = *parsed;
        next = 7;
    } else {
        write_position_error("invalid position");
        return;
    }

    GameState origin = candidate;
    std::vector<Move> parsed_moves;
    if (next < tokens.size()) {
        if (tokens[next] != "moves") {
            write_position_error("invalid position");
            return;
        }
        ++next;
        for (; next < tokens.size(); ++next) {
            const auto move = Move::parse_uci(tokens[next]);
            if (!move || !candidate.make_move(*move)) {
                write_position_error("invalid move");
                return;
            }
            parsed_moves.push_back(*move);
        }
    }

    position_origin_ = std::move(origin);
    position_moves_ = std::move(parsed_moves);
    position_ = std::move(candidate);
    if (debug_logging_enabled()) {
        std::array<Move, kMaximumLegalMoves> legal{};
        const std::size_t legal_count =
            std::min(position_.legal_moves_into(legal), legal.size());
        debug_event("position accepted generation " + std::to_string(generation_) +
                    " key " + std::to_string(position_.position_key()) +
                    " fen " + position_.fen() +
                    " legal_moves " + std::to_string(legal_count));
        const PositionConsistencySnapshot snapshot = position_.consistency_snapshot();
        debug_json_event(
            "position",
            detail::format_position_debug_json(
                generation_, command_text, snapshot, position_.en_passant_square(),
                legal_move_digest(std::span<const Move>(legal.data(), legal_count))));
    }
}

void UciController::handle_setoption(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    ProtocolMode current_protocol = ProtocolMode::uci;
    {
        std::lock_guard lock(output_mutex_);
        current_protocol = protocol_mode_;
    }
    const bool allow_ucci_short =
        current_protocol == ProtocolMode::ucci || current_protocol == ProtocolMode::ucicyclone;
    const std::optional<ParsedOption> parsed =
        parse_setoption_for_protocol(tokens, allow_ucci_short);
    if (!parsed.has_value()) {
        return;
    }
    const std::string& name = parsed->name;
    const std::string& value = parsed->value;
    const UciOptionDescriptor* option = find_uci_option(name);
    if (option == nullptr) {
        return;
    }

    // Resource options must be true no-ops when re-sent with their current
    // value: GUIs re-apply settings between games and aborting a live search for
    // an unchanged value desynchronises them.  Their handlers below assume the
    // generic helpers' changed-only contract, so filter here before dispatch.
    const bool unchanged_resource_option =
        (option->id == UciOptionId::hash && value == std::to_string(hash_mb_)) ||
        (option->id == UciOptionId::random_seed && value == std::to_string(random_seed_)) ||
        (option->id == UciOptionId::book_file && value == book_file_) ||
        (option->id == UciOptionId::syzygy_path && value == syzygy_path_.string()) ||
        (option->id == UciOptionId::eval_file && value == eval_file_.string());
    if (unchanged_resource_option) {
        if (option->id == UciOptionId::syzygy_path) {
            // The GUI re-sent the auto-detected path explicitly: record the
            // explicit choice so auto-detection can never override it later.
            syzygy_path_explicit_ = true;
            // If the auto path was detected but not loaded yet, fall through so
            // the explicit setoption performs the load now.
            if (syzygy_configured_ || !syzygy_auto_detected_) {
                return;
            }
        } else {
            return;
        }
    }

    // Type-aware helpers keep the parse -> ignore-invalid -> stop/suppress ->
    // commit sequence identical for every option: an invalid value never stops
    // the active search and never changes engine state. Re-sending the value an
    // option already has is a no-op too: GUIs routinely re-apply their settings
    // between games, and aborting a live search for nothing desynchronises them.
    const auto apply_boolean = [this, &value](bool current, const auto& commit) {
        bool parsed_boolean = false;
        if (parse_boolean(value, parsed_boolean) && parsed_boolean != current) {
            stop_and_suppress_active_search();
            commit(parsed_boolean);
        }
    };
    const auto apply_unsigned = [this, &value](const UciOptionDescriptor& descriptor,
                                               std::uint64_t current, const auto& commit) {
        std::uint64_t parsed_value = 0;
        if (parse_uint64(value, parsed_value) && parsed_value >= descriptor.minimum &&
            parsed_value <= option_maximum(descriptor) && parsed_value != current) {
            stop_and_suppress_active_search();
            commit(parsed_value);
        }
    };

    switch (option->id) {
    case UciOptionId::random_seed: {
        std::uint32_t seed = 0;
        if (parse_random_seed(value, seed) && seed != random_seed_) {
            stop_and_suppress_active_search();

            random_seed_ = seed;
        }
        break;
    }
    case UciOptionId::hash: {
        std::uint64_t megabytes = 0;
        if (parse_uint64(value, megabytes) && megabytes >= option->minimum &&
            megabytes <= option->maximum && megabytes != hash_mb_) {
            stop_and_suppress_active_search();
            try {
                const HashResizeResult result =
                    search_service_.set_hash_size_mb(static_cast<std::size_t>(megabytes));
                // Record the *effective* size: a failed or memory-capped resize
                // must leave the previous value in place so a retry with the
                // same requested size is not filtered out as a no-op.
                hash_mb_ = result.effective_mb;
                debug_json_event("hash_resize", detail::format_hash_resize_debug_json(result));
                if (result.status == HashResizeStatus::reduced ||
                    result.status == HashResizeStatus::disabled ||
                    result.reason != HashResizeReason::none) {
                    write_uci_info_string("hash requested " + std::to_string(result.requested_mb) +
                                          " MB effective " + std::to_string(result.effective_mb) +
                                          " MB status " + std::string(hash_status_name(result.status)) +
                                          " reason " + std::string(hash_reason_name(result.reason)));
                }
            } catch (...) {
                debug_event("hash resize failed outside the guarded allocation boundary");
                write_uci_info_string("hash resize failed; previous table preserved");
            }
        }
        break;
    }


    case UciOptionId::threads: {
        // The handshake advertises a dynamic maximum; a larger request must be
        // clamped (like Stockfish) instead of silently leaving Threads at 1.
        std::uint64_t requested = 0;
        if (parse_uint64(value, requested)) {
            const std::uint64_t maximum = static_cast<std::uint64_t>(maximum_search_threads());
            const std::uint64_t clamped = std::clamp<std::uint64_t>(requested, 1, maximum);
            if (clamped != threads_) {
                stop_and_suppress_active_search();
                threads_ = static_cast<std::size_t>(clamped);
            }
            if (clamped != requested) {
                write_uci_info_string("Threads " + std::to_string(requested) + " clamped to " +
                                      std::to_string(clamped));
            }
        }
        break;
    }

    case UciOptionId::analyse_mode:
        apply_boolean(analyse_mode_, [this](bool analyse_mode) { analyse_mode_ = analyse_mode; });
        break;

    case UciOptionId::multi_pv:
        apply_unsigned(*option, multi_pv_, [this](std::uint64_t multi_pv) {
            multi_pv_ = static_cast<std::size_t>(multi_pv);
        });
        break;

    case UciOptionId::ponder: {
        // Ponder tells the GUI that the engine is prepared to ponder; it does
        // not change the running search, so never abort one for this option.
        bool parsed = false;
        if (parse_boolean(value, parsed)) {
            ponder_enabled_ = parsed;
        }
        break;
    }

    case UciOptionId::own_book: {
        bool parsed = false;
        if (parse_boolean(value, parsed)) {
            own_book_explicit_ = true;
            apply_boolean(own_book_, [this](bool own_book) { own_book_ = own_book; });
        }
        break;
    }

    case UciOptionId::book_random:
        apply_boolean(book_random_, [this](bool book_random) { book_random_ = book_random; });
        break;

    case UciOptionId::book_safety:
        apply_boolean(book_safety_, [this](bool book_safety) { book_safety_ = book_safety; });
        break;

    case UciOptionId::book_safety_depth:
        apply_unsigned(*option, book_safety_depth_, [this](std::uint64_t book_safety_depth) {
            book_safety_depth_ = static_cast<std::uint8_t>(book_safety_depth);
        });
        break;

    case UciOptionId::book_file:
        if (!value.empty()) {
            stop_and_suppress_active_search();
            book_file_ = value;
            opening_book_.set_file(value);
        }
        break;

    case UciOptionId::book_depth:
        apply_unsigned(*option, book_depth_, [this](std::uint64_t book_depth) {
            book_depth_ = static_cast<std::uint8_t>(book_depth);
        });
        break;

    case UciOptionId::clear_hash:
        stop_and_suppress_active_search();
        search_service_.clear_hash();
        break;

    case UciOptionId::show_wdl:
        apply_boolean(show_wdl_, [this](bool show_wdl) { show_wdl_ = show_wdl; });
        break;

    case UciOptionId::move_overhead:
        apply_unsigned(*option, move_overhead_ms_, [this](std::uint64_t overhead) {
            move_overhead_ms_ = static_cast<std::uint32_t>(overhead);
        });
        break;

    case UciOptionId::dynamic_overhead:
        apply_boolean(dynamic_overhead_, [this](bool dynamic_overhead) {
            dynamic_overhead_ = dynamic_overhead;
        });
        break;

    case UciOptionId::slow_mover:
        apply_unsigned(*option, slow_mover_percent_, [this](std::uint64_t slow_mover) {
            slow_mover_percent_ = static_cast<std::uint32_t>(slow_mover);
        });
        break;

    case UciOptionId::contempt:
        apply_unsigned(*option, contempt_cp_, [this](std::uint64_t contempt) {
            contempt_cp_ = static_cast<std::uint32_t>(contempt);
        });
        break;

    case UciOptionId::strength_mode:
        apply_boolean(strength_mode_, [this](bool strength_mode) { strength_mode_ = strength_mode; });
        break;

    case UciOptionId::syzygy_path: {
        // Any explicit SyzygyPath (including an empty one) disables automatic
        // table discovery; the GUI is then the single authority for the path.
        syzygy_path_explicit_ = true;
        if (value == syzygy_path_.string() && syzygy_configured_) {
            break;
        }
        stop_and_suppress_active_search();
        // A single relative path resolves against the engine directory, like
        // EvalFile and BookFile.  A path list is left untouched because its
        // separators belong to Fathom's platform policy.
        std::filesystem::path resolved = value;
        const bool path_list = value.find(';') != std::string::npos ||
            value.find(':') != std::string::npos;
        if (!value.empty() && !path_list && resolved.is_relative() &&
            !executable_directory_.empty()) {
            resolved = executable_directory_ / resolved;
        }
        syzygy_path_ = resolved;
        rebuild_syzygy();
        break;
    }

    case UciOptionId::syzygy_probe_depth:
        apply_unsigned(*option, syzygy_probe_depth_, [this](std::uint64_t depth) {
            syzygy_probe_depth_ = static_cast<std::uint8_t>(depth);
            // Only the depth gate changes; keep the loaded Fathom tables and
            // any running search alive.
            if (syzygy_) {
                syzygy_->set_probe_depth(syzygy_probe_depth_);
            }
        });
        break;

    case UciOptionId::syzygy_probe_limit:
        apply_unsigned(*option, syzygy_probe_limit_, [this](std::uint64_t limit) {
            syzygy_probe_limit_ = static_cast<std::uint8_t>(limit);
            rebuild_syzygy();
        });
        break;

    case UciOptionId::syzygy_interior_depth:
        apply_unsigned(*option, syzygy_interior_depth_, [this](std::uint64_t depth) {
            // Interior probing is a search-time policy, so no tablebase rebuild
            // is needed; the value is snapshotted into each SearchOptions.
            syzygy_interior_depth_ = static_cast<std::uint8_t>(depth);
        });
        break;

    case UciOptionId::syzygy_50_move_rule:
        apply_boolean(syzygy_50_move_rule_, [this](bool fifty_move_rule) {
            syzygy_50_move_rule_ = fifty_move_rule;
            // Root/DTC selection is a policy read, not a load change.
            if (syzygy_) {
                syzygy_->set_fifty_move_rule(syzygy_50_move_rule_);
            }
        });
        break;

    case UciOptionId::eval_mode: {
        if (equals_ignore_case(value, "hce")) {
            if (!hce_active_) {
                stop_and_suppress_active_search();
                search_service_.set_evaluator(std::make_shared<ClassicalEvaluator>());
                hce_active_ = true;
            }
            write_uci_info_string("evaluator hce");
            break;
        }
        if (!equals_ignore_case(value, "nnue")) {
            break;
        }
        if (!hce_active_) {
            break;
        }
        stop_and_suppress_active_search();
        if (!eval_file_.empty()) {
            if (try_install_nnue_file(eval_file_, "EvalMode nnue")) {
                write_uci_info_string("evaluator nnue");
            }
            break;
        }
        if (boot_evaluator_ &&
            dynamic_cast<const ClassicalEvaluator*>(boot_evaluator_.get()) == nullptr) {
            search_service_.set_evaluator(boot_evaluator_);
            hce_active_ = false;
            write_uci_info_string("evaluator nnue");
        } else {
            write_uci_info_string("EvalMode nnue requires an EvalFile; keeping HCE");
        }
        break;
    }

    case UciOptionId::eval_file: {
        if (value == "<autodiscovery>") {
            // The advertised default is informational; a GUI replaying it must
            // keep the boot evaluator instead of treating it as a path.
            break;
        }
        if (value.empty()) {
            // The advertised default is empty: keep the boot-time evaluator
            // rather than failing a GUI that replays every default value.
            break;
        }
        stop_and_suppress_active_search();
        std::filesystem::path resolved;
        if (!try_install_nnue_file(value, "EvalFile", &resolved)) {
            break;
        }
        eval_file_ = value;
        if (debug_logging_enabled()) {
            debug_json_event("eval_file", "\"path\":" + debug_quoted(resolved.string()) +
                                               ",\"enabled\":true");
        }
        std::lock_guard lock(output_mutex_);
        write_uci_info_string_locked("evaluator nnue");
        write_uci_info_string_locked("NNUE enabled from " + resolved.string());
        break;
    }

    case UciOptionId::protocol: {
        const std::optional<ProtocolMode> requested_mode = parse_protocol_mode(value);
        if (!requested_mode.has_value()) {
            break;
        }
        ProtocolMode current_mode = ProtocolMode::uci;
        {
            std::lock_guard lock(output_mutex_);
            current_mode = protocol_mode_;
        }
        if (*requested_mode != current_mode) {
            stop_and_suppress_active_search();
            std::lock_guard lock(output_mutex_);
            protocol_mode_ = *requested_mode;
            xboard_force_ = false;
            xboard_analyze_ = false;
            xboard_pending_move_.reset();
        }
        break;
    }

    case UciOptionId::debug:
        apply_boolean(debug_log_->enabled(), [this](bool debug) {
            debug_log_->set_enabled(debug);
            debug_log_->configure();
        });
        break;

    case UciOptionId::debug_file:
        stop_and_suppress_active_search();
        debug_log_->set_requested_path(std::filesystem::path(value));
        if (debug_log_->enabled()) {
            debug_log_->configure();
        }
        break;
    }
}

void UciController::handle_go(std::istream& command) {
    const auto go_received = std::chrono::steady_clock::now();
    ensure_auto_syzygy_loaded();
    std::string arguments;
    std::getline(command, arguments);
    ProtocolMode current_protocol = ProtocolMode::uci;
    {
        std::lock_guard lock(output_mutex_);
        current_protocol = protocol_mode_;
    }
    SearchLimits limits =
        parse_protocol_go_limits(current_protocol, arguments, position_.side_to_move());

    if (limits.perft.has_value()) {
        // `go perft N` is a debugging command, not a search: answer with the
        // per-move node counts and the total, and no bestmove. Run it outside
        // the protocol loop so readiness and cancellation remain responsive.
        stop_and_suppress_active_search();
        start_perft(*limits.perft);
        return;
    }
    const bool has_explicit_limit = limits.depth.has_value() || limits.nodes.has_value() ||
        limits.movetime.has_value() || limits.infinite || limits.ponder;
    const bool has_side_to_move_clock = position_.side_to_move() == Color::white ?
        limits.white_clock.has_value() : limits.black_clock.has_value();
    if (!has_explicit_limit && !has_side_to_move_clock) {
        // A malformed or asymmetric clock command must still complete. The
        // TimeManager intentionally leaves a clock for the other side unset;
        // the controller supplies the same bounded fallback as bare `go`.
        limits.movetime = kBareGoFallback;
    }
    stop_and_suppress_active_search();

    if (limits.ponder) {
        ponder_origin_ = position_;
        ponder_root_ = position_;
        ponder_limits_ = limits;
        ponder_predicted_move_.reset();
        active_ponder_ = true;
    }
    start_search(position_, std::move(limits), false, false, go_received);
}

void UciController::handle_annotate(std::istream& command) {
    {
        std::lock_guard lock(output_mutex_);
        if (protocol_mode_ == ProtocolMode::xboard) {
            return;
        }
    }
    stop_and_suppress_active_search();
    ensure_auto_syzygy_loaded();

    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.empty()) {
        write_uci_info_string("annotate error: missing move or game target");
        return;
    }

    SearchLimits limits;
    if (tokens.size() > 1) {
        std::string limit_error;
        if (!parse_annotate_limits(tokens, 1, limits, limit_error)) {
            write_uci_info_string("annotate error: " + limit_error);
            return;
        }
    }

    MoveAnnotationOptions options;
    options.depth = limits.depth;
    options.movetime = limits.movetime;
    options.nodes = limits.nodes;
    options.threads = threads_;
    options.analyse_mode = true;
    options.contempt_cp = static_cast<int>(contempt_cp_);
    options.random_seed = random_seed_;
    options.syzygy = syzygy_;
    options.syzygy_interior_depth = syzygy_interior_depth_;
    options.opening_book = &opening_book_;
    options.book_enabled = own_book_ || (!own_book_explicit_ && book_is_available());
    options.book_depth = book_depth_;
    options.book_safety = book_safety_;
    options.book_safety_depth = book_safety_depth_;

    if (tokens[0] == "game") {
        if (position_moves_.empty()) {
            write_uci_info_string("annotate error: no moves in current position");
            return;
        }
        const auto annotated =
            annotate_game(search_service_, position_origin_, position_moves_, options);
        if (!annotated) {
            write_uci_info_string("annotate error: " + annotated.error());
            return;
        }
        std::lock_guard lock(output_mutex_);
        for (const MoveAnnotationResult& entry : annotated->moves) {
            write_uci_info_string_locked(format_annotate_move_line(entry));
        }
        write_uci_info_string_locked(format_annotate_summary_line("white", annotated->white));
        write_uci_info_string_locked(format_annotate_summary_line("black", annotated->black));
        return;
    }

    const std::optional<Move> played_move = detail::parse_move_uci_or_san(position_, tokens[0]);
    if (!played_move.has_value()) {
        write_uci_info_string("annotate error: invalid move " + tokens[0]);
        return;
    }

    const auto annotated =
        analyze_and_annotate_move(search_service_, position_, *played_move, options);
    if (!annotated) {
        write_uci_info_string("annotate error: " + annotated.error());
        return;
    }
    write_uci_info_string(format_annotate_move_line(*annotated));
}

void UciController::handle_display() {
    write_plain_output(detail::format_ascii_board(position_));
}

void UciController::handle_fen() {
    write_plain_output(position_.fen() + '\n');
}

void UciController::handle_epd() {
    write_plain_output(detail::format_epd(position_));
}

void UciController::handle_key() {
    write_plain_output(detail::format_key_info(position_));
}

void UciController::handle_moves(std::istream& command) {
    std::string mode;
    const bool san = (command >> mode) && (equals_ignore_case(mode, "san") || mode == "--san");
    write_plain_output(san ? detail::format_legal_moves_san(position_)
                           : detail::format_legal_moves(position_));
}

void UciController::handle_book() {
    std::string text;
    if (!book_is_available()) {
        text = "Opening book : unavailable\n";
    } else {
        const std::filesystem::path book_path = resolved_book_path();
        const std::vector<BookChoice> entries = opening_book_.probe_all(position_);
        text = detail::format_book_entries(book_path.filename().string(), entries);
    }
    write_plain_output(text);
}

void UciController::handle_tb() {
    ensure_auto_syzygy_loaded();
    write_plain_output(detail::format_syzygy_probe(syzygy_.get(), position_));
}

void UciController::handle_eval() {
    const std::shared_ptr<const Evaluator> evaluator = search_service_.evaluator();
    write_plain_output(detail::format_eval_summary(position_, evaluator.get(), hce_active_));
}

void UciController::handle_nnue() {
    const std::shared_ptr<const Evaluator> evaluator = search_service_.evaluator();
    std::string text;
    if (hce_active_) {
        text = "NNUE : inactive (EvalMode=hce)\n";
    } else if (const auto* nnue = dynamic_cast<const NnueEvaluator*>(evaluator.get());
               nnue != nullptr && nnue->network() != nullptr) {
        std::string display_name;
        if (!eval_file_.empty()) {
            display_name = display_filename(eval_file_);
        } else if (!boot_nnue_path_.empty()) {
            display_name = display_filename(boot_nnue_path_);
        } else {
            display_name = nnue_autodiscovered_ ? "<autodiscovery>" : "<loaded>";
        }
        text = detail::format_nnue_info(display_name, *nnue->network());
    } else {
        text = "NNUE : unavailable\n";
    }
    write_plain_output(text);
}

void UciController::handle_see(std::istream& command) {
    std::string token;
    if (!(command >> token)) {
        write_uci_info_string("see requires a legal UCI or SAN move (e.g. see e4d5)");
        return;
    }
    const std::optional<Move> move = detail::parse_move_uci_or_san(position_, token);
    if (!move.has_value() || !position_.is_legal(*move)) {
        write_uci_info_string("see rejected illegal move '" + token + "'");
        return;
    }
    const int gain = detail::static_exchange_gain(position_, *move);
    write_plain_output(detail::format_see_result(move->uci(), gain));
}

void UciController::handle_flip() {
    stop_and_suppress_active_search();
    if (position_.in_check()) {
        write_uci_info_string("flip rejected: position cannot legally switch sides");
        return;
    }
    const std::optional<std::string> flipped_fen = flip_position_fen(position_.fen());
    if (!flipped_fen.has_value()) {
        write_uci_info_string("flip rejected: malformed internal FEN");
        return;
    }
    const auto parsed = GameState::from_fen(*flipped_fen);
    if (!parsed.has_value()) {
        write_uci_info_string("flip rejected: position cannot legally switch sides");
        return;
    }
    position_ = *parsed;
    position_origin_ = *parsed;
    position_moves_.clear();
    write_uci_info_string(position_.side_to_move() == Color::white
                              ? "flipped side to move (white)"
                              : "flipped side to move (black)");
}

void UciController::handle_makemove(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.empty()) {
        write_uci_info_string("makemove requires at least one legal UCI or SAN move");
        return;
    }
    GameState candidate = position_;
    std::vector<Move> applied;
    applied.reserve(tokens.size());
    for (const std::string& token : tokens) {
        const std::optional<Move> move = detail::parse_move_uci_or_san(candidate, token);
        if (!move.has_value() || !candidate.is_legal(*move) || !candidate.make_move(*move)) {
            write_uci_info_string("makemove rejected illegal move '" + token + "'");
            return;
        }
        applied.push_back(*move);
    }
    stop_and_suppress_active_search();
    position_ = std::move(candidate);
    position_moves_.insert(position_moves_.end(), applied.begin(), applied.end());
    write_uci_info_string("played " + std::to_string(applied.size()) +
                          (applied.size() == 1 ? " move" : " moves"));
}

void UciController::rewind_position_history(const std::size_t plies) {
    position_moves_.resize(position_moves_.size() - plies);
    position_ = position_origin_;
    for (const Move& move : position_moves_) {
        (void)position_.make_move(move);
    }
}

void UciController::handle_undo(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.size() > 1) {
        write_uci_info_string("undo ignored unexpected trailing arguments");
        return;
    }
    std::uint64_t count = 1;
    if (tokens.size() == 1) {
        std::uint64_t parsed = 0;
        if (!parse_uint64(tokens[0], parsed) || parsed < 1) {
            write_uci_info_string("undo ignored invalid count '" + tokens[0] + "'");
            return;
        }
        count = parsed;
    }
    if (position_moves_.empty()) {
        write_uci_info_string("undo rejected: no moves to undo");
        return;
    }
    if (count > position_moves_.size()) {
        write_uci_info_string("undo rejected: only " + std::to_string(position_moves_.size()) +
                              " ply available");
        return;
    }
    stop_and_suppress_active_search();
    rewind_position_history(static_cast<std::size_t>(count));
    write_uci_info_string("undid " + std::to_string(count) +
                          (count == 1 ? " ply" : " plies"));
}

void UciController::handle_history() {
    write_plain_output(detail::format_move_history(position_origin_, position_moves_));
}

void UciController::handle_pgn() {
    write_plain_output(detail::format_pgn(position_origin_, position_moves_));
}

void UciController::handle_perft_command(std::istream& command) {
    std::string token;
    if (!(command >> token)) {
        write_uci_info_string("perft ignored invalid depth (expected 1..10 or suite)");
        return;
    }
    std::string extra;
    if (command >> extra) {
        write_uci_info_string("perft ignored unexpected trailing argument '" + extra + "'");
        return;
    }
    if (equals_ignore_case(token, "suite") || token == "--suite") {
        stop_and_suppress_active_search();
        const detail::PerftSuiteReport report = detail::run_perft_suite();
        std::lock_guard lock(output_mutex_);
        for (const std::string& line : report.lines) {
            output_ << line << '\n';
        }
        output_ << std::flush;
        return;
    }
    std::uint64_t parsed = 0;
    if (!parse_bounded_uint64(token, 1, 10, parsed)) {
        write_uci_info_string("perft ignored invalid depth (expected 1..10 or suite)");
        return;
    }
    stop_and_suppress_active_search();
    start_perft(static_cast<int>(parsed));
}

void UciController::handle_compiler() {
    write_plain_output(
        detail::format_compiler_info(KOI_ENGINE_BUILD_VERSION, KOI_ENGINE_BUILD_COMMIT));
}

void UciController::handle_bench(std::istream& command) {
    int depth = kDefaultBenchDepth;
    std::size_t hash_mb = kDefaultBenchHashMb;
    std::size_t threads = kDefaultBenchThreads;
    std::string token;
    if (command >> token) {
        std::uint64_t parsed = 0;
        if (!parse_bounded_uint64(token, 1, 64, parsed)) {
            write_uci_info_string("bench ignored invalid depth '" + token + "' (expected 1..64)");
            return;
        }
        depth = static_cast<int>(parsed);
    }
    if (command >> token) {
        std::uint64_t parsed = 0;
        if (!parse_bounded_uint64(token, 1, kMaximumHashMegabytes, parsed)) {
            write_uci_info_string("bench ignored invalid hash '" + token + "' (expected 1.." +
                                  std::to_string(kMaximumHashMegabytes) + ")");
            return;
        }
        hash_mb = static_cast<std::size_t>(parsed);
    }
    if (command >> token) {
        std::uint64_t parsed = 0;
        const std::uint64_t max_threads = static_cast<std::uint64_t>(maximum_search_threads());
        if (!parse_bounded_uint64(token, 1, max_threads, parsed)) {
            write_uci_info_string("bench ignored invalid threads '" + token + "' (expected 1.." +
                                  std::to_string(max_threads) + ")");
            return;
        }
        threads = static_cast<std::size_t>(parsed);
    }
    if (command >> token) {
        write_uci_info_string("bench ignored unexpected trailing argument '" + token + "'");
        return;
    }
    (void)run_bench(depth, hash_mb, threads);
}

void UciController::handle_wait() {
    if (active_perft_.joinable()) {
        active_perft_.join();
    }
    if (!active_search_.has_value()) {
        clear_ponder_state();
        return;
    }
    bool unbounded = false;
    {
        std::lock_guard lock(output_mutex_);
        unbounded = active_search_unbounded_;
    }
    if (unbounded) {
        write_uci_info_string("wait ignored: active search has no finite limit");
        return;
    }
    active_search_->wait();
    active_search_.reset();
    clear_ponder_state();
    std::lock_guard lock(output_mutex_);
    active_search_unbounded_ = false;
    if (state_ != ControllerState::ShuttingDown) {
        state_ = ControllerState::Idle;
    }
}

void UciController::handle_help() {
    write_plain_output(detail::format_interactive_help());
}

void UciController::handle_options() {
    const bool book_available = book_is_available();
    detail::LiveOptionsSnapshot snapshot;
    snapshot.evaluator_name = hce_active_ ? "HCE" : "NNUE";
    if (!eval_file_.empty()) {
        snapshot.eval_file = eval_file_.string();
    } else if (!boot_nnue_path_.empty()) {
        snapshot.eval_file =
            display_filename(boot_nnue_path_) + (nnue_autodiscovered_ ? " (autodiscovered)" : " (boot)");
    } else {
        snapshot.eval_file = nnue_autodiscovered_ ? "<autodiscovery>" : "<default>";
    }
    snapshot.hash_mb = hash_mb_;
    snapshot.threads = threads_;
    snapshot.multi_pv = multi_pv_;
    snapshot.ponder = ponder_enabled_;
    snapshot.analyse_mode = analyse_mode_;
    snapshot.contempt_cp = contempt_cp_;
    snapshot.move_overhead_ms = move_overhead_ms_;
    snapshot.dynamic_overhead = dynamic_overhead_;
    snapshot.slow_mover_percent = slow_mover_percent_;
    snapshot.random_seed = random_seed_;
    snapshot.own_book = own_book_;
    snapshot.own_book_explicit = own_book_explicit_;
    snapshot.book_available = book_available;
    snapshot.book_file = book_file_;
    snapshot.book_depth = book_depth_;
    snapshot.book_random = book_random_;
    snapshot.book_safety = book_safety_;
    snapshot.book_safety_depth = book_safety_depth_;
    snapshot.syzygy_path = syzygy_path_.string();
    snapshot.syzygy_auto_detected = syzygy_auto_detected_;
    snapshot.syzygy_probe_depth = syzygy_probe_depth_;
    snapshot.syzygy_probe_limit = syzygy_probe_limit_;
    snapshot.syzygy_interior_depth = syzygy_interior_depth_;
    snapshot.syzygy_50_move_rule = syzygy_50_move_rule_;

    std::lock_guard lock(output_mutex_);
    snapshot.protocol_name = protocol_mode_name(protocol_mode_);
    snapshot.show_wdl = show_wdl_;
    snapshot.observed_gui_overhead_ms = observed_gui_overhead_ms_;
    output_ << detail::format_options_summary(snapshot) << std::flush;
}

std::pair<std::uint64_t, std::int64_t> UciController::run_bench_positions(
    std::size_t position_count, std::size_t hash_mb, std::size_t threads,
    const SearchLimits& limits) {
    position_count = std::min(position_count, kBenchFens.size());
    stop_and_suppress_active_search();

    const std::size_t saved_hash_mb = search_service_.hash_size_mb();
    (void)search_service_.set_hash_size_mb(hash_mb);

    SearchOptions options;
    options.threads = threads;
    options.multi_pv = 1;
    options.hash_mb = 0;
    options.interim_report_interval = std::chrono::milliseconds{0};

    std::uint64_t total_nodes = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < position_count; ++index) {
        const auto parsed = GameState::from_fen(kBenchFens[index]);
        if (!parsed.has_value()) {
            continue;
        }
        search_service_.clear_hash();
        SearchResult completed_result;
        SearchEventSink sink;
        sink.on_complete = [&completed_result](const SearchResult& result) {
            completed_result = result;
        };
        SearchHandle handle = search_service_.start(*parsed, limits, std::move(sink), options);
        handle.wait();
        total_nodes += completed_result.stats.nodes + completed_result.stats.qnodes;
    }
    const auto elapsed_ms = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
               .count());

    (void)search_service_.set_hash_size_mb(saved_hash_mb);
    search_service_.clear_hash();
    return {total_nodes, elapsed_ms};
}

int UciController::run_bench(int depth, std::size_t hash_mb, std::size_t threads) {
    depth = std::clamp(depth, 1, 64);
    hash_mb = std::clamp<std::size_t>(hash_mb, 1, static_cast<std::size_t>(kMaximumHashMegabytes));
    threads = std::clamp<std::size_t>(threads, 1, maximum_search_threads());

    SearchLimits limits;
    limits.depth = depth;
    const auto [total_nodes, elapsed_ms] =
        run_bench_positions(kBenchFens.size(), hash_mb, threads, limits);

    write_plain_output(detail::format_bench_summary(total_nodes, elapsed_ms));
    return 0;
}

void UciController::handle_speedtest(std::istream& command) {
    std::size_t threads = kDefaultSpeedtestThreads;
    std::size_t hash_mb = kDefaultSpeedtestHashMb;
    int seconds = kDefaultSpeedtestSeconds;
    const std::uint64_t max_threads = static_cast<std::uint64_t>(maximum_search_threads());
    std::string token;
    if (command >> token) {
        std::uint64_t parsed = 0;
        if (!parse_bounded_uint64(token, 1, max_threads, parsed)) {
            write_uci_info_string("speedtest ignored invalid threads '" + token + "' (expected 1.." +
                                  std::to_string(max_threads) + ")");
            return;
        }
        threads = static_cast<std::size_t>(parsed);
    }
    if (command >> token) {
        std::uint64_t parsed = 0;
        if (!parse_bounded_uint64(token, 1, kMaximumHashMegabytes, parsed)) {
            write_uci_info_string("speedtest ignored invalid hash '" + token + "' (expected 1.." +
                                  std::to_string(kMaximumHashMegabytes) + ")");
            return;
        }
        hash_mb = static_cast<std::size_t>(parsed);
    }
    if (command >> token) {
        std::uint64_t parsed = 0;
        if (!parse_bounded_uint64(token, 1, 60, parsed)) {
            write_uci_info_string("speedtest ignored invalid seconds '" + token +
                                  "' (expected 1..60)");
            return;
        }
        seconds = static_cast<int>(parsed);
    }
    if (command >> token) {
        write_uci_info_string("speedtest ignored unexpected trailing argument '" + token + "'");
        return;
    }
    (void)run_speedtest(threads, hash_mb, seconds);
}

int UciController::run_speedtest(std::size_t threads, std::size_t hash_mb, int runtime_seconds) {
    threads = std::clamp<std::size_t>(threads, 1, maximum_search_threads());
    hash_mb = std::clamp<std::size_t>(hash_mb, 1, static_cast<std::size_t>(kMaximumHashMegabytes));
    runtime_seconds = std::clamp(runtime_seconds, 1, 60);

    const std::size_t position_count = std::min<std::size_t>(kBenchFens.size(), 4);
    SearchLimits limits;
    limits.movetime = std::chrono::milliseconds{
        std::max(100, (runtime_seconds * 1000) / static_cast<int>(std::max<std::size_t>(1, position_count)))};

    const auto [total_nodes, elapsed_ms] =
        run_bench_positions(position_count, hash_mb, threads, limits);

    write_plain_output(
        detail::format_speedtest_summary(threads, hash_mb, total_nodes, elapsed_ms));
    return 0;
}

int UciController::run_eval(const std::optional<std::string_view> fen) {
    if (fen.has_value()) {
        std::string error;
        const auto parsed = detail::parse_cli_position_spec(*fen, error);
        if (!parsed.has_value()) {
            diagnostics_ << "koi: " << error << '\n' << std::flush;
            return 2;
        }
        stop_and_suppress_active_search();
        position_ = *parsed;
        position_origin_ = *parsed;
        position_moves_.clear();
    }
    handle_eval();
    return 0;
}

int UciController::run_options() {
    ensure_auto_syzygy_loaded();
    handle_options();
    return 0;
}

void UciController::handle_ponderhit() {
    SearchLimits base_limits;
    {
        std::lock_guard lock(output_mutex_);
        if (!active_ponder_ || !ponder_limits_.has_value()) {
            return;
        }
        base_limits = *ponder_limits_;
    }

    SearchLimits converted = base_limits;
    converted.ponder = false;
    const bool has_side_to_move_clock = position_.side_to_move() == Color::white ?
        converted.white_clock.has_value() : converted.black_clock.has_value();
    const bool has_normal_limit = converted.depth.has_value() || converted.nodes.has_value() ||
        converted.movetime.has_value() || has_side_to_move_clock || converted.infinite;
    if (!has_normal_limit) {
        converted.movetime = kBareGoFallback;
    }

    if (active_search_.has_value() && active_search_->running()) {
        // Standard UCI continuation: the pondering search already searches the
        // exact root the GUI ponders after, so the ponderhit is converted in
        // place. The worker keeps its TT-warm state, identity, and generation;
        // only time management switches from unbounded to the real budget.
        const auto ponderhit_now = std::chrono::steady_clock::now();
        SearchLimits converted_for_timing = converted;
        std::uint64_t current_generation = 0;
        std::uint32_t observed_overhead = 0;
        {
            std::lock_guard lock(output_mutex_);
            current_generation = generation_;
        }
        note_clock_baseline(position_, converted_for_timing, current_generation, ponderhit_now);
        {
            std::lock_guard lock(output_mutex_);
            observed_overhead = observed_gui_overhead_ms_;
        }
        active_search_->request_ponderhit(std::move(converted), observed_overhead);
        // The worker consumes the conversion only at the next iteration
        // boundary; until then the pondering search has no deadline even though
        // the GUI clock is running. Arm a hard-budget fallback timer so a long
        // in-flight iteration cannot flag the engine.
        try {
            koi::RootTimingContext ponder_ctx;
            ponder_ctx.game_phase = position_.game_phase();
            ponder_ctx.fullmove_number = position_.fullmove_number();
            ponder_ctx.in_check = position_.in_check();
            if (position_.count_legal_moves_up_to(2) == 1) {
                ponder_ctx.legal_move_count = 1;
            }
            koi::TimeManager converted_timing(
                converted_for_timing, position_.side_to_move(), kFixedSpeedPercent,
                move_overhead_ms_, slow_mover_percent_,
                ponder_ctx, {}, dynamic_overhead_, ponderhit_now,
                observed_overhead);
            if (const auto budget = converted_timing.time_budget();
                budget.has_value() && budget->count() > 0) {
                // The timer is only a fallback until the worker consumes the
                // conversion at its next iteration boundary.  Do not add slack:
                // the budget already includes the flag-proof safety ceiling, and
                // an extra 50 ms was enough to flag a low-clock ponderhit when
                // the GUI round trip was slow.
                active_search_->stop_after(*budget);
            }
        } catch (...) {
        }
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(output_mutex_);
            ponder_origin_.reset();
            ponder_root_.reset();
            ponder_limits_.reset();
            ponder_predicted_move_.reset();
            ponder_expected_move_.reset();
            active_ponder_ = false;
            active_search_unbounded_ = converted.infinite || converted.ponder;
            state_ = ControllerState::Searching;
            generation = generation_;
        }
        debug_json_event("ponderhit",
                         "\"continued\":true,\"generation\":" + std::to_string(generation));
        return;
    }

    // No running search to convert (for example a node-limited ponder that
    // already completed). The stale ponder search is suppressed and a fresh,
    // bounded search of the real position answers this move so the GUI always
    // receives exactly one bestmove.  The restart keeps any searchmoves the
    // ponder `go` carried so the converted search has the same root.
    debug_event("ponderhit without a running ponder search; searching the current position");
    stop_and_suppress_active_search();
    start_search(position_, converted, true);
}

void UciController::note_clock_baseline(
    const GameState& root, const SearchLimits& limits, const std::uint64_t generation,
    const std::chrono::steady_clock::time_point started,
    std::uint32_t* const observed_overhead_out,
    std::optional<int>* const previous_score_out) noexcept {
    if (limits.ponder && observed_overhead_out == nullptr &&
        previous_score_out == nullptr) {
        // Speculative pondering is unbounded and precedes our next real clock
        // move or ponderhit; keep the previous clock baseline intact so ponder
        // traffic never disables GUI/network overhead learning.
        return;
    }
    const Color side_to_move = root.side_to_move();
    const std::uint16_t fullmove_number = root.fullmove_number();
    const std::uint64_t position_key = root.position_key();
    const auto capture_search_context = [this, observed_overhead_out, previous_score_out,
                                         side_to_move, fullmove_number, position_key]() noexcept {
        if (observed_overhead_out != nullptr) {
            *observed_overhead_out = observed_gui_overhead_ms_;
        }
        if (previous_score_out != nullptr) {
            if (previous_search_score_.has_value() &&
                side_to_move == previous_search_side_ &&
                (fullmove_number == static_cast<std::uint16_t>(previous_search_fullmove_ + 1U) ||
                 position_key == previous_search_position_key_)) {
                *previous_score_out = previous_search_score_;
            } else {
                previous_score_out->reset();
            }
        }
    };
    if (limits.ponder) {
        std::lock_guard lock(output_mutex_);
        capture_search_context();
        return;
    }
    const bool clock_search = !limits.infinite && !limits.movetime.has_value();
    const std::optional<ClockLimit>& clock =
        side_to_move == Color::white ? limits.white_clock : limits.black_clock;
    std::uint32_t logged_overhead = 0;
    std::int64_t logged_elapsed = 0;
    bool overhead_updated = false;
    {
        std::lock_guard lock(output_mutex_);
        capture_search_context();
        if (!clock_search || !clock.has_value()) {
            clock_baseline_valid_ = false;
            previous_clock_search_completed_ = false;
            previous_clock_fullmove_ = 0;
            previous_clock_position_key_ = 0;
            return;
        }
        // Only measure round-trip discrepancy within a continuous same-side
        // game progression (or an immediate re-search of the same position):
        // an ad-hoc FEN jump or side switch without `ucinewgame` carries an
        // unrelated clock and must not inject a false latency spike.
        const bool clock_continues_game = clock_baseline_valid_ &&
            side_to_move == previous_clock_side_ &&
            (fullmove_number == static_cast<std::uint16_t>(previous_clock_fullmove_ + 1U) ||
             position_key == previous_clock_position_key_);
        // Also measure round-trip discrepancy using only our engine's actual
        // go-to-bestmove duration; including the opponent's think time made the
        // expected clock smaller than reality and suppressed overhead learning.
        if (clock_continues_game && previous_clock_search_completed_) {
            const std::uint32_t updated = uci::derive_network_overhead_ms(
                previous_clock_remaining_ms_, previous_clock_increment_ms_,
                previous_clock_search_elapsed_ms_, clock->remaining.count(),
                observed_gui_overhead_ms_);
            if (updated != observed_gui_overhead_ms_) {
                observed_gui_overhead_ms_ = updated;
                if (observed_overhead_out != nullptr) {
                    *observed_overhead_out = updated;
                }
                logged_overhead = updated;
                logged_elapsed = previous_clock_search_elapsed_ms_;
                overhead_updated = true;
            }
        }
        previous_clock_remaining_ms_ = clock->remaining.count();
        previous_clock_increment_ms_ = clock->increment.count();
        previous_clock_search_start_ = started;
        active_clock_search_generation_ = generation;
        previous_clock_side_ = side_to_move;
        previous_clock_fullmove_ = fullmove_number;
        previous_clock_position_key_ = position_key;
        previous_clock_search_completed_ = false;
        clock_baseline_valid_ = true;
    }
    if (overhead_updated) {
        try {
            debug_event("network overhead estimate " + std::to_string(logged_overhead) +
                        " ms elapsed " + std::to_string(logged_elapsed) + " ms");
        } catch (...) {
        }
    }
}

void UciController::start_search(GameState root, SearchLimits limits, bool skip_book,
                                 bool force_analyze,
                                 std::chrono::steady_clock::time_point search_started) {
    if (search_started == std::chrono::steady_clock::time_point{}) {
        search_started = std::chrono::steady_clock::now();
    }
    const std::uint64_t generation = begin_generation();
    ProtocolMode search_protocol = ProtocolMode::uci;
    bool xboard_thinking = false;
    bool xboard_analyze_snapshot = false;
    {
        std::lock_guard lock(output_mutex_);
        state_ = limits.ponder ? ControllerState::Pondering : ControllerState::Searching;
        active_search_unbounded_ = limits.infinite || limits.ponder;
        search_protocol = protocol_mode_;
        xboard_analyze_snapshot = xboard_analyze_ || force_analyze;
        xboard_thinking = xboard_post_ || xboard_analyze_snapshot;
    }
    const auto completion_once = std::make_shared<CompletionOnce>();
    if (debug_logging_enabled()) {
        debug_event("search start generation " + std::to_string(generation));
        debug_event("search root generation " + std::to_string(generation) +
                    " key " + std::to_string(root.position_key()) +
                    " fen " + root.fen());
    }
    const bool book_eligible = !skip_book && !force_analyze && !analyse_mode_ && multi_pv_ == 1 &&
        !limits.infinite && !limits.ponder && !limits.search_moves_specified;
    std::optional<BookChoice> book_choice;
    std::uint32_t ply = 0;
    if (book_eligible) {
        ply = root_ply(root);
        // A book shipped beside the executable is used automatically unless the
        // GUI asked for OwnBook explicitly. Missing or unreadable book files are
        // silent: the search simply proceeds without one.
        const bool book_auto = !own_book_explicit_ && book_is_available();
        if (book_auto && !book_auto_reported_) {
            book_auto_reported_ = true;
            write_uci_info_string("book detected: " + resolved_book_path().string() +
                                  " (OwnBook not set, using it)");
        }
        book_choice = opening_book_.choose(root, ply, own_book_ || book_auto, book_depth_,
                                           random_seed_, book_random_, book_safety_,
                                           book_safety_depth_);
    }
    if (debug_logging_enabled()) {
        debug_json_event(
            "go",
            detail::format_go_debug_json(
                generation, root, limits, threads_, book_choice.has_value()));
    }
    if (book_choice.has_value()) {
        if (debug_logging_enabled()) {
            debug_event("book completion candidate generation " + std::to_string(generation) +
                        " move " + book_choice->move.uci() + " root " + root.fen());
        }
        write_book_completion(generation, root, limits, *book_choice, ply, completion_once,
                              search_protocol);
        return;
    }
    const GameState search_root = root;
    const bool is_ponder_search = limits.ponder;
    const bool allow_ponder_move = !limits.search_moves_specified;

    SearchEventSink sink;
    sink.on_info = [this, generation, search_root, is_ponder_search, search_protocol,
                    xboard_thinking](const SearchInfo& info) {
        SearchInfo safe_info = info;
        safe_info.pv = legal_pv_prefix(search_root, info.pv);
        if (safe_info.pv.size() != info.pv.size() && debug_logging_enabled()) {
            debug_event("invalid PV prefix suppressed generation " + std::to_string(generation));
        }
        if (safe_info.multipv == 1 && safe_info.pv.size() >= 2) {
            std::optional<GameState> ponder_after_best;
            if (is_ponder_search) {
                bool need_ponder_root = false;
                {
                    std::lock_guard lock(output_mutex_);
                    need_ponder_root =
                        generation == generation_ && !ponder_expected_move_.has_value();
                }
                if (need_ponder_root) {
                    GameState after_best = search_root;
                    if (after_best.make_move(safe_info.pv[0])) {
                        ponder_after_best = std::move(after_best);
                    }
                }
            }
            std::lock_guard lock(output_mutex_);
            if (generation == generation_) {
                principal_variation_best_move_ = safe_info.pv[0];
                principal_variation_ponder_move_ = safe_info.pv[1];
                if (is_ponder_search && !ponder_expected_move_.has_value() &&
                    ponder_after_best.has_value()) {
                    ponder_root_ = std::move(*ponder_after_best);
                    ponder_predicted_move_ = safe_info.pv[0];
                    ponder_expected_move_ = safe_info.pv[1];
                }
            }
        }
        write_search_info(generation, safe_info, search_protocol, xboard_thinking);
    };
    sink.on_complete = [this, generation, allow_ponder_move, search_root, limits, completion_once,
                        search_protocol, is_ponder_search,
                        xboard_analyze_snapshot](const SearchResult& result) {
        if (debug_logging_enabled()) {
            debug_event("search complete generation " + std::to_string(generation));
        }
        CompletionCandidate candidate;
        candidate.best_move = result.best_move;
        candidate.pv = result.pv;
        candidate.ponder_move = result.ponder_move;
        candidate.identity = result.identity;
        candidate.source = result.stats.tbhits != 0 ? CompletionSource::tablebase :
            (limits.ponder ? CompletionSource::ponder : CompletionSource::search);
        const SearchRequestIdentity expected = SearchRequestIdentity::from(search_root, generation);
        std::string last_command;
        {
            std::lock_guard lock(output_mutex_);
            if (generation == generation_ && allow_ponder_move && candidate.best_move.has_value() &&
                candidate.best_move == principal_variation_best_move_) {
                candidate.ponder_move = principal_variation_ponder_move_;
            }
            if (debug_logging_enabled()) {
                last_command = last_command_;
            }
        }
        const CompletionValidation validation = completion_gate_.validate(
            search_root, limits, expected, candidate);
        if (debug_logging_enabled()) {
            const std::string source = result.stats.tbhits != 0 ? "tablebase" :
                (limits.ponder ? "ponder" : "search");
            debug_json_event(
                "completion_validation",
                detail::format_search_completion_debug_json(
                    generation, search_root, candidate, validation, result, source, last_command));
        }
        if (validation.disposition == CompletionDisposition::suppress_stale) {
            if (debug_logging_enabled()) {
                debug_event("stale search completion suppressed generation " +
                            std::to_string(generation) + " result_generation " +
                            std::to_string(result.identity.generation) + " result_key " +
                            std::to_string(result.identity.root_key));
            }
            return;
        }
        if (validation.fallback_used && debug_logging_enabled()) {
            debug_event("illegal or shadow-rejected search move replaced generation " +
                        std::to_string(generation) + " fallback " +
                        (validation.best_move.has_value() ? validation.best_move->uci() : "0000"));
        }
        if (validation.disposition == CompletionDisposition::quarantine) {
            // UCI requires exactly one answer per `go`, and a protocol or
            // rules disagreement must never kill the engine: claim the
            // completion, report the condition on the info channel, and answer
            // 0000. The engine stays alive for the next command; the previous
            // quick_exit(74) turned a recoverable root inconsistency into
            // engine death.
            if (debug_logging_enabled()) {
                debug_event("no common legal move for nonterminal search root generation " +
                            std::to_string(generation) + " fen " + search_root.fen() +
                            "; answering 0000");
            }
            if (!completion_once->try_claim()) {
                if (debug_logging_enabled()) {
                    debug_event("duplicate search completion suppressed generation " +
                                std::to_string(generation));
                }
                return;
            }
            std::lock_guard lock(output_mutex_);
            if (generation != generation_) {
                return;
            }
            if (is_ponder_search) {
                clear_ponder_state();
            }
            if (search_protocol != ProtocolMode::xboard) {
                output_ << "info string no common legal move for the search root; answering 0000\n"
                        << std::flush;
            }
            write_no_move_reply(output_, search_protocol, search_root);
            return;
        }
        if (!completion_once->try_claim()) {
            if (debug_logging_enabled()) {
                debug_event("duplicate search completion suppressed generation " +
                            std::to_string(generation));
            }
            return;
        }
        SearchResult completed = result;
        completed.best_move = validation.best_move;
        completed.pv = validation.pv;
        completed.ponder_move = validation.ponder_move;
        if (debug_logging_enabled()) {
            const std::string_view source = result.stats.tbhits != 0 ? "tablebase" :
                (limits.ponder ? "ponder" : "search");
            debug_json_event(
                "move",
                detail::format_search_move_debug_json(
                    generation, search_root, validation.best_move, source));
        }
        if (result.failed) {
            // A setup/internal failure still answers with a legal fallback move,
            // but it must not be indistinguishable from a real result at the
            // protocol boundary.
            std::lock_guard lock(output_mutex_);
            if (generation == generation_ && search_protocol != ProtocolMode::xboard) {
                output_ << "info string search failed ("
                        << search_failure_reason_name(result.failure_reason)
                        << "); using a safe fallback move\n"
                        << std::flush;
            }
        }
        write_search_completion(generation, completed, search_protocol, is_ponder_search,
                                xboard_analyze_snapshot, search_root);
    };

    std::uint32_t observed_overhead = 0;
    std::optional<int> previous_score;
    note_clock_baseline(search_root, limits, generation, search_started,
                        &observed_overhead, &previous_score);

    SearchOptions options;
    options.threads = threads_;
    options.multi_pv = multi_pv_;
    options.random_seed = random_seed_;
    options.analyse_mode = analyse_mode_ || force_analyze;
    options.show_wdl = show_wdl_;
    options.move_overhead_ms = move_overhead_ms_;
    options.dynamic_move_overhead = dynamic_overhead_;
    options.observed_overhead_ms = observed_overhead;
    options.previous_search_score = previous_score;
    options.slow_mover_percent = slow_mover_percent_;
    options.contempt_cp = static_cast<int>(contempt_cp_);
    options.generation = generation;
    options.strength_mode = strength_mode_;
    options.syzygy = syzygy_;
    options.syzygy_interior_depth = syzygy_interior_depth_;
    options.search_started = search_started;
    const bool has_search_moves = limits.search_moves_specified;
    const std::vector<Move> fallback_search_moves =
        has_search_moves ? limits.search_moves : std::vector<Move>{};
    try {
        active_search_.emplace(
            search_service_.start(std::move(root), std::move(limits), std::move(sink), options));
    } catch (const std::exception& error) {
        // Defer root legal move generation to the rare setup-failure path so
        // normal timed searches do not spend start latency building moves twice.
        SearchResult start_failure_fallback;
        start_failure_fallback.identity = SearchRequestIdentity::from(search_root, generation);
        start_failure_fallback.completed = true;
        start_failure_fallback.failed = true;
        start_failure_fallback.failure_reason = SearchFailureReason::setup_failed;
        std::array<Move, kMaximumLegalMoves> fallback_moves{};
        const std::size_t move_count =
            std::min(search_root.legal_moves_into(fallback_moves), fallback_moves.size());
        for (std::size_t index = 0; index < move_count; ++index) {
            const Move& move = fallback_moves[index];
            if (!has_search_moves ||
                std::find(fallback_search_moves.begin(), fallback_search_moves.end(), move) !=
                    fallback_search_moves.end()) {
                start_failure_fallback.best_move = move;
                start_failure_fallback.pv = {move};
                break;
            }
        }
        debug_event("search failed to start: " + std::string(error.what()));
        {
            std::lock_guard lock(output_mutex_);
            clear_ponder_state();
            if (search_protocol != ProtocolMode::xboard) {
                output_ << "info string search could not start: " << error.what() << '\n'
                        << "info string search failed (setup_failed); using a safe fallback "
                           "move\n"
                        << std::flush;
            }
        }
        write_search_completion(generation, start_failure_fallback, search_protocol, is_ponder_search,
                                xboard_analyze_snapshot, search_root);
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::ShuttingDown) {
            state_ = ControllerState::Idle;
        }
    }
}

void UciController::stop_active_search() {
    {
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::ShuttingDown) {
            state_ = ControllerState::Stopping;
        }
    }
    stop_active_perft();
    if (!active_search_.has_value()) {
        clear_ponder_state();
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::ShuttingDown) state_ = ControllerState::Idle;
        return;
    }

    debug_event("search cancellation requested");
    active_search_->stop();
    active_search_->wait();
    active_search_.reset();
    clear_ponder_state();
    std::lock_guard lock(output_mutex_);
    active_search_unbounded_ = false;
    if (state_ != ControllerState::ShuttingDown) state_ = ControllerState::Idle;
}

void UciController::stop_and_suppress_active_search() {
    {
        std::lock_guard lock(output_mutex_);
        ++generation_;
        if (state_ != ControllerState::ShuttingDown) {
            state_ = ControllerState::Stopping;
        }
    }
    debug_event("search cancellation requested with generation suppression");
    stop_active_perft();
    if (active_search_.has_value()) {
        active_search_->stop();
        active_search_->wait();
        active_search_.reset();
    }
    clear_ponder_state();
    {
        std::lock_guard lock(output_mutex_);
        active_search_unbounded_ = false;
        if (state_ != ControllerState::ShuttingDown) state_ = ControllerState::Idle;
    }
}

void UciController::start_perft(const int depth) {
    const std::uint64_t generation = begin_generation();
    {
        std::lock_guard lock(output_mutex_);
        if (state_ == ControllerState::ShuttingDown) {
            return;
        }
        state_ = ControllerState::Searching;
    }

    try {
        perft_running_.store(true, std::memory_order_release);
        active_perft_ = std::jthread(
            [this, root = position_, depth, generation](const std::stop_token stop_token) mutable {
                struct RunningFlagReset {
                    std::atomic_bool& flag;
                    ~RunningFlagReset() {
                        flag.store(false, std::memory_order_release);
                    }
                } running_flag_reset{perft_running_};
                try {
                    write_perft_results(std::move(root), depth, stop_token, generation);
                } catch (...) {
                    debug_event("perft worker failed");
                    std::lock_guard lock(output_mutex_);
                    if (generation == generation_ &&
                        state_ != ControllerState::ShuttingDown) {
                        write_uci_info_string_locked("perft failed");
                        state_ = ControllerState::Idle;
                    }
                }
            });
    } catch (...) {
        perft_running_.store(false, std::memory_order_release);
        debug_event("perft worker failed to start");
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::ShuttingDown) {
            write_uci_info_string_locked("perft could not start");
            state_ = ControllerState::Idle;
        }
    }
}

void UciController::stop_active_perft() {
    if (active_perft_.joinable()) {
        active_perft_.request_stop();
        active_perft_.join();
    }
}

void UciController::clear_ponder_state() {
    ponder_origin_.reset();
    ponder_root_.reset();
    ponder_limits_.reset();
    ponder_predicted_move_.reset();
    ponder_expected_move_.reset();
    active_ponder_ = false;
}

std::uint64_t UciController::begin_generation() {
    std::lock_guard lock(output_mutex_);
    principal_variation_best_move_.reset();
    principal_variation_ponder_move_.reset();
    ponder_expected_move_.reset();
    return ++generation_;
}

void UciController::write_handshake(const ProtocolMode mode) {
    std::lock_guard lock(output_mutex_);
    output_ << "id name Koi " << KOI_ENGINE_BUILD_VERSION << '\n'
            << "id author Koi team\n";
    for (const UciOptionDescriptor& option : kUciOptions) {
        if (!option.advertised) {
            continue;
        }
        output_ << "option name " << option.name << " type ";
        switch (option.kind) {
        case UciOptionKind::spin:
            output_ << "spin default " << option.default_value << " min " << option.minimum
                    << " max " << option_maximum(option);
            break;
        case UciOptionKind::check:
            output_ << "check default " << option.default_value;
            break;
        case UciOptionKind::string:
            if (option.id == UciOptionId::eval_file && nnue_autodiscovered_) {
                // Informational only: the boot evaluator was installed by
                // auto-discovery, not by a user-supplied path.
                output_ << "string default <autodiscovery>";
            } else {
                output_ << "string default " << option.default_value;
            }
            break;
        case UciOptionKind::button:
            output_ << "button";
            break;
        case UciOptionKind::combo:
            output_ << "combo default "
                    << (option.id == UciOptionId::protocol ? protocol_mode_name(mode)
                                                           : option.default_value);
            if (!option.combo_values.empty()) {
                output_ << ' ' << option.combo_values;
            }
            break;
        }
        output_ << '\n';
    }
    output_ << "info string build " << KOI_ENGINE_BUILD_COMMIT << '\n';
    switch (mode) {
    case ProtocolMode::usi:
        output_ << "info string USI protocol compatibility mode (standard 8x8 chess only)\n"
                << "usiok\n";
        break;
    case ProtocolMode::ucci:
    case ProtocolMode::ucicyclone:
        output_ << "info string UCCI protocol compatibility mode ("
                << protocol_mode_name(mode) << ", standard 8x8 chess only)\n"
                << "ucciok\n";
        break;
    case ProtocolMode::uci:
    case ProtocolMode::xboard:
        output_ << "uciok\n";
        break;
    }
    output_ << std::flush;
}

void UciController::write_xboard_features() {
    std::lock_guard lock(output_mutex_);
    output_ << "feature myname=\"Koi " << KOI_ENGINE_BUILD_VERSION
            << "\" setboard=1 ping=1 usermove=1 colors=0 reuse=1 analyze=1 memory=1 smp=1 "
               "variants=\"normal\" sigint=0 sigterm=0 done=1\n"
            << std::flush;
}

std::filesystem::path UciController::resolved_book_path() const {
    std::filesystem::path path{book_file_};
    if (path.is_relative() && !executable_directory_.empty()) {
        path = executable_directory_ / path;
    }
    return path;
}

bool UciController::book_is_available() const {
    const std::filesystem::path path = resolved_book_path();
    if (!OpeningBook::is_usable_file(path)) {
        return false;
    }
    // A file can be present and correctly sized but unreadable (ACL deny,
    // permissions, another process); opening here keeps the auto-detection
    // announcement truthful instead of claiming a book that `choose()` cannot
    // read. No negative cache: a later readable book is detected on the next
    // search.
    std::ifstream stream(path, std::ios::binary);
    return static_cast<bool>(stream);
}

void UciController::write_readyok() {
    ensure_auto_syzygy_loaded();
    // The production UCI controller starts with a one-megabyte bootstrap table
    // so a large default hash cannot consume a fast game's clock before the
    // GUI has delivered its options. Materialize the requested table while the
    // engine is idle; explicit Hash options have already resized it here.
    if (!active_search_.has_value() &&
        !perft_running_.load(std::memory_order_acquire)) {
        try {
            (void)search_service_.set_hash_size_mb(hash_mb_);
        } catch (...) {
            debug_event("deferred hash materialization failed; existing table preserved");
        }
    }
    std::lock_guard lock(output_mutex_);
    output_ << "readyok\n" << std::flush;
}

void UciController::write_perft_results(GameState root, const int depth,
                                        const std::stop_token stop_token,
                                        const std::uint64_t generation) {
    root.detach_mirror();
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::pair<Move, std::uint64_t>> counts;
    std::array<Move, kMaximumLegalMoves> legal_moves{};
    const std::size_t move_count =
        std::min(root.legal_moves_into(legal_moves), legal_moves.size());
    counts.reserve(move_count);
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < move_count; ++index) {
        const Move& move = legal_moves[index];
        if (stop_token.stop_requested()) {
            debug_event("perft cancelled before completion");
            return;
        }
        GameState child = root;
        if (!child.make_move(move)) {
            continue;
        }
        const PerftResult result = koi::perft_interruptible(child, depth - 1, stop_token);
        if (result.cancelled) {
            debug_event("perft cancelled before completion");
            return;
        }
        total += result.nodes;
        counts.emplace_back(move, result.nodes);
    }
    const auto elapsed_ms = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
               .count());
    const std::vector<std::string> divide_lines =
        detail::format_perft_divide_lines(counts, total, elapsed_ms);
    debug_event("perft depth " + std::to_string(depth) + " nodes " + std::to_string(total));
    std::lock_guard lock(output_mutex_);
    if (stop_token.stop_requested() || generation != generation_ ||
        state_ == ControllerState::ShuttingDown) {
        return;
    }
    if (protocol_mode_ != ProtocolMode::xboard) {
        for (const std::string& line : divide_lines) {
            write_uci_info_string_locked(line);
        }
    }
    state_ = ControllerState::Idle;
}

void UciController::write_search_info(const std::uint64_t generation, const SearchInfo& info,
                                      const ProtocolMode protocol, const bool xboard_thinking) {
    std::lock_guard lock(output_mutex_);
    if (generation != generation_ || info.depth <= 0 ||
        state_ == ControllerState::ShuttingDown) {
        return;
    }
    std::array<char, 5> move_buffer{};
    if (protocol == ProtocolMode::xboard) {
        if (!xboard_thinking || info.multipv != 1) {
            return;
        }
        const int score = info.mate.has_value() ? cecp_mate_score(*info.mate) : info.score_cp;
        const auto centiseconds = info.elapsed.count() / 10;
        output_ << info.depth << ' ' << score << ' ' << centiseconds << ' ' << info.nodes;
        for (const Move& move : info.pv) {
            output_ << ' ' << move.format_uci(move_buffer);
        }
        output_ << '\n' << std::flush;
        return;
    }

    output_ << "info depth " << info.depth << " seldepth " << info.seldepth
            << " multipv " << info.multipv;
    if (!info.currmove.is_no_move()) {
        output_ << " currmove " << info.currmove.format_uci(move_buffer);
        if (info.currmovenumber > 0) {
            // Standard UCI pairs the in-flight root move with its one-based
            // index; SearchInfo carries both (populated by the runner).
            output_ << " currmovenumber " << info.currmovenumber;
        }
    }
    // UCI has no "estimate" marker: an unproven selective-root score is still
    // the engine's best current evaluation, and GUIs (En Croissant analysis,
    // Arena, ...) show 0 cp when the token is omitted.  Publish it unmarked;
    // the internal SearchInfo::Bound::estimate flag remains authoritative for
    // search code and must never be treated as exact there.
    output_ << " score ";
    if (info.mate.has_value()) {
        output_ << "mate " << *info.mate;
    } else {
        output_ << "cp " << info.score_cp;
    }
    if (info.bound == SearchInfo::Bound::lower) {
        output_ << " lowerbound";
    } else if (info.bound == SearchInfo::Bound::upper) {
        output_ << " upperbound";
    }
    if (show_wdl_ && info.bound != SearchInfo::Bound::estimate) {
        const Wdl wdl = score_to_wdl(info);
        output_ << " wdl " << wdl.win << ' ' << wdl.draw << ' ' << wdl.loss;
    }
    output_ << " nodes " << info.nodes << " nps " << info.nps
            << " hashfull " << search_service_.hashfull_permill()
            << " time " << info.elapsed.count() << " pv";
    for (const Move& move : info.pv) {
        output_ << ' ' << move.format_uci(move_buffer);
    }
    if (info.tbhits != 0) {
        output_ << " tbhits " << info.tbhits;
    }
    output_ << '\n' << std::flush;
}

void UciController::ensure_auto_syzygy_loaded() {
    if (!syzygy_auto_detected_ || syzygy_auto_loaded_ || syzygy_path_explicit_) {
        return;
    }
    {
        // Never re-initialise the process-global Fathom registry while a search
        // may be probing it; a later isready/go (with no active search) retries.
        std::lock_guard lock(output_mutex_);
        if (state_ != ControllerState::Idle) {
            return;
        }
    }
    syzygy_auto_loaded_ = true;
    write_uci_info_string("syzygy detected: " + syzygy_path_.string() +
                          " (SyzygyPath not set, using it)");
    rebuild_syzygy();
}

void UciController::rebuild_syzygy(const bool emit_diagnostics) {
    syzygy_configured_ = true;
    syzygy_.reset();
    // Every configured directory is validated before Fathom sees it: Fathom
    // registers tables by filename and would otherwise report a corrupt file
    // only at probe time (on stderr).  This applies to single directories and
    // to every component of a platform path list.
    const std::string raw_path = syzygy_path_.string();
    const bool path_list = raw_path.find(kSyzygyPathSeparator) != std::string::npos;
    if (!syzygy_path_.empty()) {
        const auto reject_unusable = [this, emit_diagnostics](const std::filesystem::path& bad,
                                                              SyzygyDirectoryStatus status) {
            if (emit_diagnostics) {
                write_uci_info_string(
                    std::string("Syzygy: ") +
                    (status == SyzygyDirectoryStatus::unreadable ? "unreadable" : "corrupt") +
                    " tables at " + bad.string() + " (probing disabled)");
            }
            syzygy_ = std::make_shared<SyzygyTablebase>();
        };
        const auto unusable = [](SyzygyDirectoryStatus status) {
            return status == SyzygyDirectoryStatus::corrupt ||
                   status == SyzygyDirectoryStatus::unreadable;
        };
        if (!path_list) {
            const SyzygyDirectoryStatus status = syzygy_directory_status(syzygy_path_);
            if (unusable(status)) {
                reject_unusable(syzygy_path_, status);
                return;
            }
        } else {
            std::size_t begin = 0;
            while (begin <= raw_path.size()) {
                const std::size_t end = raw_path.find(kSyzygyPathSeparator, begin);
                const std::size_t length =
                    (end == std::string::npos ? raw_path.size() : end) - begin;
                if (length != 0) {
                    const std::filesystem::path component =
                        std::filesystem::path(raw_path.substr(begin, length));
                    const SyzygyDirectoryStatus status = syzygy_directory_status(component);
                    if (unusable(status)) {
                        reject_unusable(component, status);
                        return;
                    }
                }
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1;
            }
        }
    }
    syzygy_ = std::make_shared<SyzygyTablebase>(
        syzygy_path_, syzygy_probe_limit_, syzygy_probe_depth_, syzygy_50_move_rule_);
    if (syzygy_path_.empty() || !emit_diagnostics) {
        return;
    }
    // Surface load diagnostics and the largest registered table so a GUI can
    // tell an empty/mismatched directory apart from a working install.
    switch (syzygy_->load_status()) {
    case SyzygyLoadStatus::loaded:
        write_uci_info_string("Syzygy: " + std::to_string(syzygy_->large_table_limit()) +
                              "-man tables at " + syzygy_path_.string());
        break;
    case SyzygyLoadStatus::path_conflict:
        // Fathom is process-global and already owns a different path.  Report
        // the ownership conflict instead of pretending the directory is empty.
        write_uci_info_string(
            "Syzygy: path conflicts with an already-loaded tablebase (probing disabled)");
        break;
    case SyzygyLoadStatus::init_failed:
        write_uci_info_string("Syzygy: failed to initialize tables at " + syzygy_path_.string() +
                              " (probing disabled)");
        break;
    case SyzygyLoadStatus::disabled:
    case SyzygyLoadStatus::empty_path:
    case SyzygyLoadStatus::path_not_directory:
    case SyzygyLoadStatus::no_tables:
        write_uci_info_string("Syzygy: no usable tables at " + syzygy_path_.string());
        break;
    }
}

void UciController::write_search_completion(const std::uint64_t generation,
                                            const SearchResult& result,
                                            const ProtocolMode protocol,
                                            const bool is_ponder_search,
                                            const bool xboard_analyze,
                                            const GameState& search_root) {
    std::lock_guard lock(output_mutex_);
    if (generation != generation_ || state_ == ControllerState::ShuttingDown) {
        return;
    }
    std::array<char, 5> move_buffer{};
    if (protocol == ProtocolMode::xboard) {
        if (is_ponder_search || xboard_analyze) {
            return;
        }
        if (result.best_move.has_value()) {
            xboard_pending_move_ = result.best_move;
            xboard_pending_move_generation_ = generation;
            output_ << "move " << result.best_move->format_uci(move_buffer) << '\n' << std::flush;
        } else {
            output_ << xboard_terminal_result_line(search_root) << '\n' << std::flush;
        }
        return;
    }

    if (!result.best_move.has_value()) {
        write_no_move_reply(output_, protocol, search_root);
        return;
    }

    output_ << "bestmove " << result.best_move->format_uci(move_buffer);
    // The ponder move is part of the answer whenever the engine knows it; the
    // `Ponder` option only tells the engine whether the GUI will actually
    // ponder, so gating the move on it needlessly starved GUIs of the hint.
    if (result.ponder_move.has_value()) {
        output_ << " ponder " << result.ponder_move->format_uci(move_buffer);
    }
    output_ << '\n' << std::flush;
    const bool matched_clock_baseline =
        generation == active_clock_search_generation_ && clock_baseline_valid_;
    if (matched_clock_baseline) {
        previous_clock_search_elapsed_ms_ = std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - previous_clock_search_start_)
                   .count());
        previous_clock_search_completed_ = true;
    }
    if ((!is_ponder_search || generation == active_clock_search_generation_) &&
        result.completed_depth > 0 && !result.mate.has_value()) {
        previous_search_score_ = result.score_cp;
        if (matched_clock_baseline) {
            previous_search_side_ = previous_clock_side_;
            previous_search_fullmove_ = previous_clock_fullmove_;
            previous_search_position_key_ = previous_clock_position_key_;
        } else {
            previous_search_side_ = search_root.side_to_move();
            previous_search_fullmove_ = search_root.fullmove_number();
            previous_search_position_key_ = search_root.position_key();
        }
    }
}

void UciController::write_book_completion(const std::uint64_t generation, const GameState& root,
                                           const SearchLimits& limits, const BookChoice& choice,
                                           const std::uint32_t ply,
                                           const std::shared_ptr<CompletionOnce>& completion_once,
                                           const ProtocolMode protocol) {
    CompletionCandidate candidate;
    candidate.best_move = choice.move;
    candidate.pv = {choice.move};
    candidate.identity = SearchRequestIdentity::from(root, generation);
    candidate.source = CompletionSource::book;
    const CompletionValidation validation = completion_gate_.validate(
        root, limits, candidate.identity, candidate);
    std::string last_command;
    {
        std::lock_guard lock(output_mutex_);
        last_command = last_command_;
    }
    if (debug_logging_enabled()) {
        debug_json_event(
            "completion_validation",
            detail::format_book_completion_debug_json(
                generation, root, candidate, validation, choice.move, last_command));
    }
    if (validation.disposition == CompletionDisposition::suppress_stale) {
        debug_event("book completion suppressed as stale generation " + std::to_string(generation));
        return;
    }
    if (validation.disposition == CompletionDisposition::quarantine) {
        // Mirror the search completion path: never exit the process for a
        // rules/protocol disagreement, answer the one bestmove the GUI expects,
        // and keep serving the next command.
        debug_event("no common legal move for nonterminal book root generation " +
                    std::to_string(generation) + " fen " + root.fen() + "; answering 0000");
        if (!completion_once->try_claim()) {
            debug_event("duplicate book completion suppressed generation " +
                        std::to_string(generation));
            return;
        }
        std::lock_guard lock(output_mutex_);
        if (generation != generation_) {
            return;
        }
        if (protocol != ProtocolMode::xboard) {
            output_ << "info string no common legal move for the book root; answering 0000\n"
                    << std::flush;
        }
        write_no_move_reply(output_, protocol, root);
        return;
    }
    if (!completion_once->try_claim()) {
        debug_event("duplicate book completion suppressed generation " + std::to_string(generation));
        return;
    }
    if (!validation.best_move.has_value()) {
        // Defensive hardening: a validated book completion must never be
        // dereferenced without a move. Mirrors the search path, which answers
        // 0000 so a `go` always receives exactly one bestmove.
        debug_event("book completion without a validated move generation " +
                    std::to_string(generation) + "; answering 0000");
        std::lock_guard lock(output_mutex_);
        if (generation != generation_) {
            return;
        }
        write_no_move_reply(output_, protocol, root);
        return;
    }
    if (debug_logging_enabled()) {
        debug_json_event(
            "move",
            detail::format_book_move_debug_json(generation, root, validation.best_move, ply));
    }
    std::lock_guard lock(output_mutex_);
    if (generation != generation_) {
        return;
    }

    std::array<char, 5> move_buffer{};
    const std::string_view best_uci = validation.best_move->format_uci(move_buffer);
    if (protocol == ProtocolMode::xboard) {
        xboard_pending_move_ = validation.best_move;
        xboard_pending_move_generation_ = generation;
        output_ << "move " << best_uci << '\n' << std::flush;
        return;
    }

    output_ << "info string book move " << best_uci << " depth " << ply << '\n'
             << "bestmove " << best_uci << '\n' << std::flush;
}

bool UciController::debug_logging_enabled() const noexcept {
    return debug_log_ && debug_log_->enabled();
}

bool UciController::try_install_nnue_file(const std::filesystem::path& path,
                                          const std::string_view option_label,
                                          std::filesystem::path* resolved_out) {
    // Relative paths resolve against the engine directory so a GUI can pass a
    // bare filename sitting next to the executable.
    std::filesystem::path resolved = path;
    if (resolved.is_relative() && !executable_directory_.empty()) {
        resolved = executable_directory_ / resolved;
    }
    std::expected<NnueNetwork, NnueError> network = NnueLoader::load_file(resolved);
    if (!network.has_value()) {
        debug_event(std::string(option_label) + " rejected " + resolved.string() + ": " +
                    network.error().message);
        write_uci_info_string(std::string(option_label) + " rejected: " +
                              network.error().message);
        return false;
    }
    try {
        auto weights = std::make_shared<const NnueNetwork>(std::move(*network));
        search_service_.set_evaluator(std::make_shared<NnueEvaluator>(weights));
        hce_active_ = false;
    } catch (const std::exception& install_error) {
        // Installing a network allocates; a failure here must keep the previous
        // evaluator and keep the protocol loop alive.
        debug_event(std::string(option_label) + " installation failed " + resolved.string() +
                    ": " + install_error.what());
        write_uci_info_string(std::string(option_label) + " rejected: " + install_error.what());
        return false;
    }
    if (resolved_out != nullptr) {
        *resolved_out = std::move(resolved);
    }
    return true;
}

void UciController::debug_event(std::string message) noexcept {
    if (!debug_logging_enabled()) {
        return;
    }
    debug_json_event("log", "\"message\":" + debug_quoted(message));
}

void UciController::debug_json_event(std::string event, std::string fields) noexcept {
    if (debug_logging_enabled()) {
        debug_log_->write_event(event, fields);
    }
}

void UciController::write_uci_info_string_locked(const std::string_view message) {
    if (protocol_mode_ == ProtocolMode::xboard) {
        return;
    }
    output_ << "info string " << message << '\n' << std::flush;
}

void UciController::write_uci_info_string(const std::string_view message) {
    std::lock_guard lock(output_mutex_);
    write_uci_info_string_locked(message);
}

void UciController::write_plain_output(const std::string_view text) {
    std::lock_guard lock(output_mutex_);
    output_ << text << std::flush;
}

void UciController::write_position_error(const char* message) {
    write_uci_info_string(message);
    diagnostics_ << "UCI position error: " << message << '\n';
}

} // namespace koi
