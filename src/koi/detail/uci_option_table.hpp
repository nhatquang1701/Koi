#pragma once

// UCI option catalog: the single source of truth for the advertised option
// order, defaults and bounds, shared by the handshake writer and the
// setoption handler.  Kept private (detail/) because it is protocol data, not
// an embedding API.

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "koi/detail/search_constants.hpp"

namespace koi::detail {

inline constexpr std::uint64_t kMaximumRandomSeed = 2'147'483'647;
inline constexpr std::uint64_t kMinimumHashMegabytes = 1;
inline constexpr std::uint64_t kMaximumHashMegabytes = 4'096;
inline constexpr std::uint64_t kMaximumContemptCp = 1'000;
inline constexpr std::uint64_t kMaximumBookDepth = 40;
inline constexpr std::uint64_t kMaximumBookSafetyDepth = 3;
inline constexpr std::uint64_t kMinimumSyzygyProbeDepth = 1;
inline constexpr std::uint64_t kMaximumSyzygyProbeDepth = 100;
inline constexpr std::uint64_t kMaximumSyzygyProbeLimit = 7;
inline constexpr std::uint64_t kMinimumSyzygyInteriorOptionDepth = 0;
inline constexpr std::uint64_t kMaximumSyzygyInteriorDepth = 100;

enum class UciOptionKind { check, spin, string, button, combo };

enum class UciOptionId {
    random_seed,
    hash,
    threads,
    analyse_mode,
    multi_pv,
    ponder,
    own_book,
    book_file,
    book_depth,
    book_random,
    book_safety,
    book_safety_depth,
    clear_hash,
    show_wdl,
    move_overhead,
    dynamic_overhead,
    slow_mover,
    contempt,
    strength_mode,
    syzygy_path,
    syzygy_probe_depth,
    syzygy_probe_limit,
    syzygy_interior_depth,
    syzygy_50_move_rule,
    eval_file,
    eval_mode,
    debug,
    debug_file,
};

struct UciOptionDescriptor {
    std::string_view name;
    UciOptionKind kind;
    UciOptionId id;
    std::string_view default_value;
    std::uint64_t minimum;
    std::uint64_t maximum;
    bool dynamic_maximum;
    bool advertised;
    std::string_view combo_values;
};

inline constexpr std::array<UciOptionDescriptor, 28> kUciOptions{{
    {"RandomSeed", UciOptionKind::spin, UciOptionId::random_seed, "0", 0,
     kMaximumRandomSeed, false, true},
    {"Hash", UciOptionKind::spin, UciOptionId::hash, "512", kMinimumHashMegabytes,
     kMaximumHashMegabytes, false, true},
    {"Threads", UciOptionKind::spin, UciOptionId::threads, "1", 1, 0, true, true},
    {"UCI_AnalyseMode", UciOptionKind::check, UciOptionId::analyse_mode, "false", 0, 0, false,
     true},
    {"MultiPV", UciOptionKind::spin, UciOptionId::multi_pv, "1", kMinimumMultiPv,
     kMaximumMultiPv, false, true},
    {"Ponder", UciOptionKind::check, UciOptionId::ponder, "false", 0, 0, false, true},
    {"OwnBook", UciOptionKind::check, UciOptionId::own_book, "false", 0, 0, false, true},
    {"BookFile", UciOptionKind::string, UciOptionId::book_file, "book.bin", 0, 0, false, true},
    {"BookDepth", UciOptionKind::spin, UciOptionId::book_depth, "16", 0, kMaximumBookDepth,
     false, true},
    {"BookRandom", UciOptionKind::check, UciOptionId::book_random, "false", 0, 0, false, true},
    {"BookSafety", UciOptionKind::check, UciOptionId::book_safety, "true", 0, 0, false, true},
    {"BookSafetyDepth", UciOptionKind::spin, UciOptionId::book_safety_depth, "2", 0,
     kMaximumBookSafetyDepth, false, true},
    {"Clear Hash", UciOptionKind::button, UciOptionId::clear_hash, "", 0, 0, false, true},
    {"UCI_ShowWDL", UciOptionKind::check, UciOptionId::show_wdl, "false", 0, 0, false, true},
    {"Move Overhead", UciOptionKind::spin, UciOptionId::move_overhead, "30", 0,
     kMaximumMoveOverheadMs, false, true},
    {"Dynamic Overhead", UciOptionKind::check, UciOptionId::dynamic_overhead, "false", 0, 0,
     false, true},
    {"Slow Mover", UciOptionKind::spin, UciOptionId::slow_mover, "100", kMinimumSlowMoverPercent,
     kMaximumSlowMoverPercent, false, true},
    {"Contempt", UciOptionKind::spin, UciOptionId::contempt, "0", 0, kMaximumContemptCp, false,
     true},
    {"StrengthMode", UciOptionKind::check, UciOptionId::strength_mode, "false", 0, 0, false, true},
    {"SyzygyPath", UciOptionKind::string, UciOptionId::syzygy_path, "", 0, 0, false, true},
    {"SyzygyProbeDepth", UciOptionKind::spin, UciOptionId::syzygy_probe_depth, "1",
     kMinimumSyzygyProbeDepth, kMaximumSyzygyProbeDepth, false, true},
    {"SyzygyProbeLimit", UciOptionKind::spin, UciOptionId::syzygy_probe_limit, "7", 0,
     kMaximumSyzygyProbeLimit, false, true},
    {"SyzygyInteriorDepth", UciOptionKind::spin, UciOptionId::syzygy_interior_depth, "0",
     kMinimumSyzygyInteriorOptionDepth, kMaximumSyzygyInteriorDepth, false, true},
    {"Syzygy50MoveRule", UciOptionKind::check, UciOptionId::syzygy_50_move_rule, "true", 0, 0,
     false, true},
    {"EvalFile", UciOptionKind::string, UciOptionId::eval_file, "", 0, 0, false, true},
    {"EvalMode", UciOptionKind::combo, UciOptionId::eval_mode, "nnue", 0, 0, false, true,
     "var nnue var hce"},
    {"Debug", UciOptionKind::check, UciOptionId::debug, "false", 0, 0, false, false},
    {"DebugFile", UciOptionKind::string, UciOptionId::debug_file, "koi-debug.log", 0, 0, false,
     false},
}};

[[nodiscard]] inline const UciOptionDescriptor* find_uci_option(std::string_view name) noexcept {
    for (const UciOptionDescriptor& option : kUciOptions) {
        if (option.name.size() != name.size()) {
            continue;
        }
        bool equal = true;
        for (std::size_t index = 0; index < name.size(); ++index) {
            if (std::tolower(static_cast<unsigned char>(option.name[index])) !=
                std::tolower(static_cast<unsigned char>(name[index]))) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return &option;
        }
    }
    return nullptr;
}

} // namespace koi::detail
