#include "koi/syzygy_tablebase.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

#include <tbprobe.h>

namespace koi {

namespace {

std::mutex& fathom_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct FathomOwnership {
    std::filesystem::path path;
    std::size_t users = 0;
};

FathomOwnership& fathom_ownership() {
    static FathomOwnership ownership;
    return ownership;
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

#if defined(_WIN32)
constexpr char kSyzygyPathSeparator = ';';
#else
constexpr char kSyzygyPathSeparator = ':';
#endif

// Fathom's tb_init has an out-of-bounds read in its path-list splitter when the
// list ends with a separator (or contains only separators): it allocates
// strlen+1 bytes, overwrites every separator with NUL, then scans for a
// non-empty component past the terminator.  A user can send `SyzygyPath ;;;`
// or a plain trailing separator, so the list is normalised here first: empty
// components are dropped and a list with no components becomes empty (which
// Fathom treats as "no tables").  This is fail-closed and does not change the
// meaning of any valid component.
std::string sanitized_path_list(std::string_view text) {
    std::string result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(kSyzygyPathSeparator, begin);
        const std::size_t length =
            (end == std::string_view::npos ? text.size() : end) - begin;
        if (length != 0) {
            if (!result.empty()) {
                result.push_back(kSyzygyPathSeparator);
            }
            result.append(text.substr(begin, length));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return result;
}

bool is_path_list(std::string_view text) {
    return text.find(kSyzygyPathSeparator) != std::string_view::npos;
}

SyzygyLoadStatus acquire_fathom(const std::string& raw_path) {
    const std::string path_text = sanitized_path_list(raw_path);
    if (path_text.empty()) {
        return SyzygyLoadStatus::empty_path;
    }
    std::lock_guard lock(fathom_mutex());
    FathomOwnership& ownership = fathom_ownership();
    // A path list is not a directory: canonicalising the whole string would
    // mangle it before Fathom's own splitter sees it.
    const bool path_list = is_path_list(path_text);
    const std::filesystem::path normalized =
        path_list ? std::filesystem::path(path_text) : normalized_path(path_text);
    if (ownership.users != 0) {
        if (ownership.path != normalized) {
            return SyzygyLoadStatus::path_conflict;
        }
        ++ownership.users;
        return SyzygyLoadStatus::loaded;
    }

    const bool initialized = tb_init(normalized.string().c_str());
    if (!initialized) {
        return SyzygyLoadStatus::init_failed;
    }
    if (TB_LARGEST == 0) {
        tb_free();
        return SyzygyLoadStatus::no_tables;
    }
    ownership.path = normalized;
    ownership.users = 1;
    return SyzygyLoadStatus::loaded;
}

void release_fathom() noexcept {
    std::lock_guard lock(fathom_mutex());
    FathomOwnership& ownership = fathom_ownership();
    if (ownership.users == 0) {
        return;
    }
    --ownership.users;
    if (ownership.users == 0) {
        tb_free();
        ownership.path.clear();
    }
}

std::optional<Move> move_from_fathom(const TbMove move) noexcept {
    const Square from = Square::from_index(static_cast<std::uint8_t>(TB_MOVE_FROM(move)));
    const Square to = Square::from_index(static_cast<std::uint8_t>(TB_MOVE_TO(move)));
    Promotion promotion = Promotion::none;
    switch (TB_MOVE_PROMOTES(move)) {
    case TB_PROMOTES_KNIGHT: promotion = Promotion::knight; break;
    case TB_PROMOTES_BISHOP: promotion = Promotion::bishop; break;
    case TB_PROMOTES_ROOK: promotion = Promotion::rook; break;
    case TB_PROMOTES_QUEEN: promotion = Promotion::queen; break;
    case TB_PROMOTES_NONE: break;
    default: return std::nullopt;
    }
    return Move{from, to, promotion};
}

void native_bitboards(const TablebaseSnapshot& snapshot, std::uint64_t& white,
                      std::uint64_t& black, std::uint64_t& kings, std::uint64_t& queens,
                      std::uint64_t& rooks, std::uint64_t& bishops, std::uint64_t& knights,
                      std::uint64_t& pawns) noexcept {
    white = black = kings = queens = rooks = bishops = knights = pawns = 0;
    for (std::size_t color = 0; color < 2; ++color) {
        const std::uint64_t color_bits = snapshot.piece_bitboards[color][0] |
            snapshot.piece_bitboards[color][1] | snapshot.piece_bitboards[color][2] |
            snapshot.piece_bitboards[color][3] | snapshot.piece_bitboards[color][4] |
            snapshot.piece_bitboards[color][5];
        if (color == 0) white = color_bits; else black = color_bits;
        kings |= snapshot.piece_bitboards[color][5];
        queens |= snapshot.piece_bitboards[color][4];
        rooks |= snapshot.piece_bitboards[color][3];
        bishops |= snapshot.piece_bitboards[color][2];
        knights |= snapshot.piece_bitboards[color][1];
        pawns |= snapshot.piece_bitboards[color][0];
    }
}

unsigned native_ep(const TablebaseSnapshot& snapshot) noexcept {
    return snapshot.en_passant_square.index() < Square::kInvalid ?
        static_cast<unsigned>(snapshot.en_passant_square.index()) : 0;
}

bool valid_snapshot(const TablebaseSnapshot& snapshot) noexcept {
    if (snapshot.side_to_move != Color::white && snapshot.side_to_move != Color::black) {
        return false;
    }

    std::uint64_t occupied = 0;
    for (const auto& colors : snapshot.piece_bitboards) {
        for (const std::uint64_t pieces : colors) {
            if ((occupied & pieces) != 0) {
                return false;
            }
            occupied |= pieces;
        }
    }

    const std::uint64_t white_kings =
        snapshot.piece_bitboards[0][static_cast<std::size_t>(PieceType::king) - 1];
    const std::uint64_t black_kings =
        snapshot.piece_bitboards[1][static_cast<std::size_t>(PieceType::king) - 1];
    if (std::popcount(white_kings) != 1 || std::popcount(black_kings) != 1) {
        return false;
    }
    const int white_king = std::countr_zero(white_kings);
    const int black_king = std::countr_zero(black_kings);
    const int file_delta = (white_king & 7) - (black_king & 7);
    const int rank_delta = (white_king >> 3) - (black_king >> 3);
    if (file_delta >= -1 && file_delta <= 1 && rank_delta >= -1 && rank_delta <= 1) {
        return false;
    }

    constexpr std::uint64_t kBackRanks = 0xFF000000000000FFULL;
    const std::uint64_t pawns = snapshot.piece_bitboards[0][0] | snapshot.piece_bitboards[1][0];
    if ((pawns & kBackRanks) != 0) {
        return false;
    }

    if (snapshot.en_passant_square.index() >= Square::kInvalid) {
        return true;
    }
    if (snapshot.halfmove_clock != 0) {
        return false;
    }

    const int target = snapshot.en_passant_square.index();
    const bool white_to_move = snapshot.side_to_move == Color::white;
    if ((white_to_move && (target >> 3) != 5) || (!white_to_move && (target >> 3) != 2)) {
        return false;
    }
    const std::uint64_t target_bit = std::uint64_t{1} << target;
    if ((occupied & target_bit) != 0) {
        return false;
    }

    const int pawn_square = target + (white_to_move ? -8 : 8);
    const int origin_square = target + (white_to_move ? 8 : -8);
    if (pawn_square < 0 || pawn_square >= 64 || origin_square < 0 || origin_square >= 64) {
        return false;
    }
    const std::size_t pawn_color = white_to_move ? 1 : 0;
    const std::uint64_t pawn_bit = std::uint64_t{1} << pawn_square;
    const std::uint64_t origin_bit = std::uint64_t{1} << origin_square;
    return (snapshot.piece_bitboards[pawn_color][0] & pawn_bit) != 0 &&
        (occupied & origin_bit) == 0;
}

} // namespace

class SyzygyTablebase::Impl {
public:
    std::filesystem::path path;
    std::uint8_t probe_limit = 7;
    std::atomic<std::uint8_t> probe_depth{1};
    std::atomic_bool fifty_move_rule{true};
    bool enabled = false;
    SyzygyLoadStatus status = SyzygyLoadStatus::disabled;
    std::atomic<std::uint64_t> hits = 0;
};

SyzygyScore syzygy_score(const SyzygyWdl wdl) noexcept {
    switch (wdl) {
    case SyzygyWdl::win:
        // Stay inside the 90 000 tablebase band: kMateThreshold is 99 000 and
        // consumers classify >= that as a mate score.
        return {90'000, 1};
    case SyzygyWdl::loss:
        return {-90'000, -1};
    case SyzygyWdl::cursed_win:
        return {1, std::nullopt};
    case SyzygyWdl::blessed_loss:
        return {-1, std::nullopt};
    case SyzygyWdl::draw:
        return {0, std::nullopt};
    }
    return {};
}

SyzygyScore syzygy_root_score(const SyzygyWdl wdl) noexcept {
    switch (wdl) {
    case SyzygyWdl::win:
        return {90'000, std::nullopt};
    case SyzygyWdl::loss:
        return {-90'000, std::nullopt};
    default:
        // Draws, cursed wins, and blessed losses keep their near-zero scores
        // and never advertise a mate distance.
        return syzygy_score(wdl);
    }
}

std::array<int, 3> syzygy_wdl_permill(const SyzygyWdl wdl) noexcept {
    switch (wdl) {
    case SyzygyWdl::win:
        return {1'000, 0, 0};
    case SyzygyWdl::loss:
        return {0, 0, 1'000};
    case SyzygyWdl::cursed_win:
    case SyzygyWdl::blessed_loss:
    case SyzygyWdl::draw:
        return {0, 1'000, 0};
    }
    return {0, 1'000, 0};
}

SyzygyWdl syzygy_wdl_from_rank(const int rank) noexcept {
    // Fathom's root ranks use |rank| >= 1000 for a win that converts inside the
    // 50-move rule and 900..999 for a DTZ-optimal win that only converts if the
    // opponent cooperates (v + cnt50 > 99 or a repetition). Its own root probe
    // treats |rank| >= 900 as decisive, so classify on the same boundary instead
    // of demoting 900..999 to cursed wins.
    if (rank >= 900) return SyzygyWdl::win;
    if (rank > 0) return SyzygyWdl::cursed_win;
    if (rank <= -900) return SyzygyWdl::loss;
    if (rank < 0) return SyzygyWdl::blessed_loss;
    return SyzygyWdl::draw;
}

SyzygyTablebase::SyzygyTablebase(std::filesystem::path path, const std::uint8_t probe_limit,
                                 const std::uint8_t probe_depth, const bool fifty_move_rule)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    // The vendored Fathom snapshot supports seven-man tables, so the default
    // probes every table the user may have installed (Stockfish defaults to 7
    // as well); an explicit lower limit is still honoured.
    impl_->probe_limit = std::clamp<std::uint8_t>(probe_limit, 0, 7);
    impl_->probe_depth.store(std::clamp<std::uint8_t>(probe_depth, 1, 100),
                             std::memory_order_relaxed);
    impl_->fifty_move_rule.store(fifty_move_rule, std::memory_order_relaxed);
    std::error_code path_error;
    // Fathom accepts a platform path list and splits it itself (SEP_CHAR:
    // ';' on Windows, ':' elsewhere).  A list is not a directory, so the
    // single-directory readability gate must not reject it up front.
    const std::string path_text = impl_->path.string();
#if defined(_WIN32)
    const bool path_list = path_text.find(';') != std::string::npos;
#else
    const bool path_list = path_text.find(':') != std::string::npos;
#endif
    const bool readable_directory =
        path_list || std::filesystem::is_directory(impl_->path, path_error);
    if (impl_->path.empty()) {
        impl_->status = SyzygyLoadStatus::empty_path;
        return;
    }
    if (impl_->probe_limit == 0) {
        impl_->status = SyzygyLoadStatus::disabled;
        return;
    }
    if (path_error || !readable_directory) {
        impl_->status = SyzygyLoadStatus::path_not_directory;
        return;
    }
    impl_->status = acquire_fathom(impl_->path.string());
    impl_->enabled = impl_->status == SyzygyLoadStatus::loaded;
}

SyzygyTablebase::~SyzygyTablebase() {
    if (impl_->enabled) {
        release_fathom();
    }
}

bool SyzygyTablebase::enabled() const noexcept { return impl_->enabled; }

SyzygyLoadStatus SyzygyTablebase::load_status() const noexcept { return impl_->status; }

int SyzygyTablebase::large_table_limit() const noexcept {
    return impl_->enabled ? static_cast<int>(TB_LARGEST) : 0;
}

bool SyzygyTablebase::probe_eligible(const GameState& state) const noexcept {
    if (!impl_->enabled || state.castling_rights() != 0) {
        return false;
    }
    // The GameState bitboards are maintained incrementally, so this stays far
    // cheaper than building a TablebaseSnapshot at every interior node.
    std::size_t pieces = 0;
    for (std::uint8_t type = static_cast<std::uint8_t>(PieceType::pawn);
         type <= static_cast<std::uint8_t>(PieceType::king); ++type) {
        const auto piece_type = static_cast<PieceType>(type);
        pieces += static_cast<std::size_t>(std::popcount(state.piece_bitboard(piece_type, Color::white)));
        pieces += static_cast<std::size_t>(std::popcount(state.piece_bitboard(piece_type, Color::black)));
    }
    if (pieces == 0 || pieces > impl_->probe_limit || pieces > 7) {
        return false;
    }
    const int largest = large_table_limit();
    return largest > 0 && pieces <= static_cast<std::size_t>(largest);
}

bool SyzygyTablebase::supports(const TablebaseSnapshot& snapshot) const noexcept {
    return valid_snapshot(snapshot) && snapshot.castling_rights == 0 &&
        snapshot.piece_count() <= impl_->probe_limit &&
        snapshot.piece_count() <= 7;
}

bool SyzygyTablebase::allows_depth(const int depth) const noexcept {
    // Minimum-depth gate: probe once the search reaches the requested depth.
    return depth >= static_cast<int>(impl_->probe_depth.load(std::memory_order_relaxed));
}

void SyzygyTablebase::set_probe_depth(const std::uint8_t depth) noexcept {
    impl_->probe_depth.store(std::clamp<std::uint8_t>(depth, 1, 100),
                             std::memory_order_relaxed);
}

void SyzygyTablebase::set_fifty_move_rule(const bool enabled) noexcept {
    impl_->fifty_move_rule.store(enabled, std::memory_order_relaxed);
}

std::uint8_t SyzygyTablebase::probe_limit() const noexcept { return impl_->probe_limit; }
std::uint8_t SyzygyTablebase::probe_depth() const noexcept {
    return impl_->probe_depth.load(std::memory_order_relaxed);
}
bool SyzygyTablebase::fifty_move_rule() const noexcept {
    return impl_->fifty_move_rule.load(std::memory_order_relaxed);
}
bool SyzygyTablebase::uses_clock_aware_root_probe() const noexcept {
    return impl_->fifty_move_rule.load(std::memory_order_relaxed);
}

std::optional<SyzygyWdl> SyzygyTablebase::probe_wdl(
    const TablebaseSnapshot& snapshot) const noexcept {
    if (!enabled() || !supports(snapshot) ||
        (impl_->fifty_move_rule.load(std::memory_order_relaxed) && snapshot.halfmove_clock != 0)) {
        return std::nullopt;
    }
    std::uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    native_bitboards(snapshot, white, black, kings, queens, rooks, bishops, knights, pawns);
    // tb_probe_wdl is documented as thread safe (third_party/fathom/tbprobe.h),
    // so interior probes may run concurrently without the process lock.
    const unsigned result = tb_probe_wdl(white, black, kings, queens, rooks, bishops,
                                         knights, pawns, 0, snapshot.castling_rights,
                                         native_ep(snapshot), snapshot.side_to_move == Color::white);
    if (result == TB_RESULT_FAILED) {
        return std::nullopt;
    }
    impl_->hits.fetch_add(1, std::memory_order_relaxed);
    switch (TB_GET_WDL(result)) {
    case TB_LOSS: return SyzygyWdl::loss;
    case TB_BLESSED_LOSS: return SyzygyWdl::blessed_loss;
    case TB_DRAW: return SyzygyWdl::draw;
    case TB_CURSED_WIN: return SyzygyWdl::cursed_win;
    case TB_WIN: return SyzygyWdl::win;
    default: return std::nullopt;
    }
}

std::optional<SyzygyRootResult> SyzygyTablebase::probe_root(
    const GameState& state, const std::vector<Move>& allowed_moves) const noexcept {
    const TablebaseSnapshot snapshot = state.tablebase_snapshot();
    if (!enabled() || !supports(snapshot)) {
        return std::nullopt;
    }
    std::uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    native_bitboards(snapshot, white, black, kings, queens, rooks, bishops, knights, pawns);
    TbRootMoves root_moves{};
    int probe_succeeded = 0;
    {
        // tb_probe_root is documented as NOT thread safe, so root probes stay
        // serialised against each other and against Fathom lifetime changes.
        std::lock_guard lock(fathom_mutex());
        if (impl_->fifty_move_rule.load(std::memory_order_relaxed)) {
            probe_succeeded = tb_probe_root_dtz(
                white, black, kings, queens, rooks, bishops, knights, pawns,
                snapshot.halfmove_clock, snapshot.castling_rights,
                native_ep(snapshot), snapshot.side_to_move == Color::white,
                state.is_repetition_sensitive(), true, &root_moves);
            if (probe_succeeded == 0) {
                root_moves = {};
                probe_succeeded = tb_probe_root_wdl(
                    white, black, kings, queens, rooks, bishops, knights, pawns,
                    snapshot.halfmove_clock, snapshot.castling_rights,
                    native_ep(snapshot), snapshot.side_to_move == Color::white,
                    true, &root_moves);
            }
        } else {
            probe_succeeded = tb_probe_root_wdl(
                white, black, kings, queens, rooks, bishops, knights, pawns,
                snapshot.halfmove_clock, snapshot.castling_rights,
                native_ep(snapshot), snapshot.side_to_move == Color::white,
                false, &root_moves);
        }
    }
    if (probe_succeeded == 0 || root_moves.size == 0 || root_moves.size > TB_MAX_MOVES) {
        return std::nullopt;
    }
    const auto permitted = [&allowed_moves](const Move& move) {
        return allowed_moves.empty() || std::find(allowed_moves.begin(), allowed_moves.end(), move) !=
            allowed_moves.end();
    };
    int best_rank = std::numeric_limits<int>::min();
    std::vector<Move> best_moves;
    for (unsigned index = 0; index < root_moves.size; ++index) {
        const auto move = move_from_fathom(root_moves.moves[index].move);
        if (!move.has_value() || !permitted(*move) || !state.is_legal(*move)) {
            continue;
        }
        const int rank = root_moves.moves[index].tbRank;
        if (rank > best_rank) {
            best_rank = rank;
            best_moves.clear();
        }
        if (rank == best_rank) {
            best_moves.push_back(*move);
        }
    }
    if (best_moves.empty()) {
        return std::nullopt;
    }
    const SyzygyWdl wdl = syzygy_wdl_from_rank(best_rank);
    SyzygyRootResult result{wdl, syzygy_root_score(wdl), std::move(best_moves)};
    impl_->hits.fetch_add(1, std::memory_order_relaxed);
    return result;
}

std::uint64_t SyzygyTablebase::hits() const noexcept {
    return impl_->hits.load(std::memory_order_relaxed);
}

namespace {

struct StandardSyzygyFile {
    std::string_view name;
    std::uint64_t size;
};

// Canonical file sizes of the standard 3-4-5 man Syzygy set (290 files: 145 WDL
// + 145 DTZ).  Fathom trusts the compressed-table header; a truncated file that
// still satisfies Fathom's 16-mod-64 size congruence can hang a probe.  Koi
// rejects size mismatches before handing a directory to Fathom.  Unknown names
// (e.g. 6-man tables) are not size-validated.
constexpr std::array<StandardSyzygyFile, 290> kStandardSyzygyFiles{{
    {"KBBBvK.rtbw", 739600},
    {"KBBBvK.rtbz", 1938576},
    {"KBBNvK.rtbw", 106832},
    {"KBBNvK.rtbz", 9450128},
    {"KBBPvK.rtbw", 1167824},
    {"KBBPvK.rtbz", 1075152},
    {"KBBvK.rtbw", 58000},
    {"KBBvK.rtbz", 136272},
    {"KBBvKB.rtbw", 351824},
    {"KBBvKB.rtbz", 53072},
    {"KBBvKN.rtbw", 5064400},
    {"KBBvKN.rtbz", 8136912},
    {"KBBvKP.rtbw", 7783952},
    {"KBBvKP.rtbz", 6765008},
    {"KBBvKQ.rtbw", 2217040},
    {"KBBvKQ.rtbz", 7933776},
    {"KBBvKR.rtbw", 574864},
    {"KBBvKR.rtbz", 89360},
    {"KBNNvK.rtbw", 92496},
    {"KBNNvK.rtbz", 9450128},
    {"KBNPvK.rtbw", 478800},
    {"KBNPvK.rtbz", 556880},
    {"KBNvK.rtbw", 7632},
    {"KBNvK.rtbz", 461840},
    {"KBNvKB.rtbw", 716816},
    {"KBNvKB.rtbz", 114576},
    {"KBNvKN.rtbw", 3575952},
    {"KBNvKN.rtbz", 1826640},
    {"KBNvKP.rtbw", 10146256},
    {"KBNvKP.rtbz", 32367120},
    {"KBNvKQ.rtbw", 1054544},
    {"KBNvKQ.rtbz", 12536144},
    {"KBNvKR.rtbw", 1328144},
    {"KBNvKR.rtbz", 118480},
    {"KBPPvK.rtbw", 274128},
    {"KBPPvK.rtbz", 116624},
    {"KBPvK.rtbw", 81424},
    {"KBPvK.rtbz", 75792},
    {"KBPvKB.rtbw", 15290512},
    {"KBPvKB.rtbz", 3497488},
    {"KBPvKN.rtbw", 20381328},
    {"KBPvKN.rtbz", 7242128},
    {"KBPvKP.rtbw", 13821584},
    {"KBPvKP.rtbz", 7172304},
    {"KBPvKQ.rtbw", 7497872},
    {"KBPvKQ.rtbz", 23906768},
    {"KBPvKR.rtbw", 14246864},
    {"KBPvKR.rtbz", 1573776},
    {"KBvK.rtbw", 80},
    {"KBvK.rtbz", 80},
    {"KBvKB.rtbw", 1232},
    {"KBvKB.rtbz", 80},
    {"KBvKN.rtbw", 2256},
    {"KBvKN.rtbz", 80},
    {"KBvKP.rtbw", 107472},
    {"KBvKP.rtbz", 5968},
    {"KNNNvK.rtbw", 85712},
    {"KNNNvK.rtbz", 3821712},
    {"KNNPvK.rtbw", 882576},
    {"KNNPvK.rtbz", 1262288},
    {"KNNvK.rtbw", 1360},
    {"KNNvK.rtbz", 80},
    {"KNNvKB.rtbw", 67024},
    {"KNNvKB.rtbz", 30544},
    {"KNNvKN.rtbw", 81360},
    {"KNNvKN.rtbz", 32912},
    {"KNNvKP.rtbw", 13729360},
    {"KNNvKP.rtbz", 10331216},
    {"KNNvKQ.rtbw", 3241744},
    {"KNNvKQ.rtbz", 4940624},
    {"KNNvKR.rtbw", 313680},
    {"KNNvKR.rtbz", 62928},
    {"KNPPvK.rtbw", 186512},
    {"KNPPvK.rtbz", 111312},
    {"KNPvK.rtbw", 93200},
    {"KNPvK.rtbz", 111056},
    {"KNPvKB.rtbw", 13148688},
    {"KNPvKB.rtbz", 4021776},
    {"KNPvKN.rtbw", 14200272},
    {"KNPvKN.rtbz", 4930192},
    {"KNPvKP.rtbw", 18616208},
    {"KNPvKP.rtbz", 8974160},
    {"KNPvKQ.rtbw", 10056720},
    {"KNPvKQ.rtbz", 15554128},
    {"KNPvKR.rtbw", 19810256},
    {"KNPvKR.rtbz", 4874960},
    {"KNvK.rtbw", 80},
    {"KNvK.rtbz", 80},
    {"KNvKN.rtbw", 1168},
    {"KNvKN.rtbz", 80},
    {"KNvKP.rtbw", 148048},
    {"KNvKP.rtbz", 10640},
    {"KPPPvK.rtbw", 56080},
    {"KPPPvK.rtbz", 22672},
    {"KPPvK.rtbw", 25104},
    {"KPPvK.rtbz", 8656},
    {"KPPvKB.rtbw", 7269520},
    {"KPPvKB.rtbz", 517520},
    {"KPPvKN.rtbw", 7707472},
    {"KPPvKN.rtbz", 699472},
    {"KPPvKP.rtbw", 7499088},
    {"KPPvKP.rtbz", 669776},
    {"KPPvKQ.rtbw", 3090576},
    {"KPPvKQ.rtbz", 3358672},
    {"KPPvKR.rtbw", 9068688},
    {"KPPvKR.rtbz", 5114512},
    {"KPvK.rtbw", 7824},
    {"KPvK.rtbz", 4176},
    {"KPvKP.rtbw", 245328},
    {"KPvKP.rtbz", 54480},
    {"KQBBvK.rtbw", 160464},
    {"KQBBvK.rtbz", 3094096},
    {"KQBNvK.rtbw", 255888},
    {"KQBNvK.rtbz", 2967312},
    {"KQBPvK.rtbw", 824272},
    {"KQBPvK.rtbz", 1347600},
    {"KQBvK.rtbw", 4944},
    {"KQBvK.rtbz", 139088},
    {"KQBvKB.rtbw", 232656},
    {"KQBvKB.rtbz", 6845584},
    {"KQBvKN.rtbw", 387664},
    {"KQBvKN.rtbz", 4369104},
    {"KQBvKP.rtbw", 2925136},
    {"KQBvKP.rtbz", 7444880},
    {"KQBvKQ.rtbw", 4092496},
    {"KQBvKQ.rtbz", 1260112},
    {"KQBvKR.rtbw", 684048},
    {"KQBvKR.rtbz", 8740752},
    {"KQNNvK.rtbw", 113680},
    {"KQNNvK.rtbz", 3680656},
    {"KQNPvK.rtbw", 648784},
    {"KQNPvK.rtbz", 1295184},
    {"KQNvK.rtbw", 3600},
    {"KQNvK.rtbz", 156432},
    {"KQNvKB.rtbw", 218320},
    {"KQNvKB.rtbz", 8035600},
    {"KQNvKN.rtbw", 333456},
    {"KQNvKN.rtbz", 5411152},
    {"KQNvKP.rtbw", 2662096},
    {"KQNvKP.rtbz", 7902992},
    {"KQNvKQ.rtbw", 3139152},
    {"KQNvKQ.rtbz", 785296},
    {"KQNvKR.rtbw", 759824},
    {"KQNvKR.rtbz", 10843024},
    {"KQPPvK.rtbw", 255568},
    {"KQPPvK.rtbz", 132240},
    {"KQPvK.rtbw", 12496},
    {"KQPvK.rtbz", 32208},
    {"KQPvKB.rtbw", 785744},
    {"KQPvKB.rtbz", 2151824},
    {"KQPvKN.rtbw", 1694352},
    {"KQPvKN.rtbz", 2055632},
    {"KQPvKP.rtbw", 2464016},
    {"KQPvKP.rtbz", 997904},
    {"KQPvKQ.rtbw", 20403600},
    {"KQPvKQ.rtbz", 16695824},
    {"KQPvKR.rtbw", 2030736},
    {"KQPvKR.rtbz", 6202512},
    {"KQQBvK.rtbw", 227600},
    {"KQQBvK.rtbz", 806160},
    {"KQQNvK.rtbw", 186064},
    {"KQQNvK.rtbz", 920848},
    {"KQQPvK.rtbw", 725200},
    {"KQQPvK.rtbz", 583760},
    {"KQQQvK.rtbw", 165648},
    {"KQQQvK.rtbz", 207248},
    {"KQQRvK.rtbw", 247696},
    {"KQQRvK.rtbz", 610512},
    {"KQQvK.rtbw", 7056},
    {"KQQvK.rtbz", 25936},
    {"KQQvKB.rtbw", 32976},
    {"KQQvKB.rtbz", 1086480},
    {"KQQvKN.rtbw", 63952},
    {"KQQvKN.rtbz", 1028688},
    {"KQQvKP.rtbw", 372240},
    {"KQQvKP.rtbz", 1878992},
    {"KQQvKQ.rtbw", 666832},
    {"KQQvKQ.rtbz", 2793424},
    {"KQQvKR.rtbw", 82256},
    {"KQQvKR.rtbz", 1261584},
    {"KQRBvK.rtbw", 280720},
    {"KQRBvK.rtbz", 2226576},
    {"KQRNvK.rtbw", 211920},
    {"KQRNvK.rtbz", 2435344},
    {"KQRPvK.rtbw", 907600},
    {"KQRPvK.rtbz", 1170832},
    {"KQRRvK.rtbw", 200528},
    {"KQRRvK.rtbz", 880848},
    {"KQRvK.rtbw", 4560},
    {"KQRvK.rtbz", 44112},
    {"KQRvKB.rtbw", 159568},
    {"KQRvKB.rtbz", 2664848},
    {"KQRvKN.rtbw", 250832},
    {"KQRvKN.rtbz", 1878608},
    {"KQRvKP.rtbw", 984976},
    {"KQRvKP.rtbz", 4687056},
    {"KQRvKQ.rtbw", 3095888},
    {"KQRvKQ.rtbz", 9245968},
    {"KQRvKR.rtbw", 279824},
    {"KQRvKR.rtbz", 3421968},
    {"KQvK.rtbw", 272},
    {"KQvK.rtbz", 5392},
    {"KQvKB.rtbw", 6672},
    {"KQvKB.rtbz", 195216},
    {"KQvKN.rtbw", 10064},
    {"KQvKN.rtbz", 162640},
    {"KQvKP.rtbw", 58064},
    {"KQvKP.rtbz", 205776},
    {"KQvKQ.rtbw", 16528},
    {"KQvKQ.rtbz", 6736},
    {"KQvKR.rtbw", 20496},
    {"KQvKR.rtbz", 309008},
    {"KRBBvK.rtbw", 134416},
    {"KRBBvK.rtbz", 6354384},
    {"KRBNvK.rtbw", 207696},
    {"KRBNvK.rtbz", 9500944},
    {"KRBPvK.rtbw", 531600},
    {"KRBPvK.rtbz", 951632},
    {"KRBvK.rtbw", 2832},
    {"KRBvK.rtbz", 261776},
    {"KRBvKB.rtbw", 916624},
    {"KRBvKB.rtbz", 20128272},
    {"KRBvKN.rtbw", 998224},
    {"KRBvKN.rtbz", 17507152},
    {"KRBvKP.rtbw", 5300624},
    {"KRBvKP.rtbz", 9324304},
    {"KRBvKQ.rtbw", 9588688},
    {"KRBvKQ.rtbz", 5323152},
    {"KRBvKR.rtbw", 2035280},
    {"KRBvKR.rtbz", 575440},
    {"KRNNvK.rtbw", 98832},
    {"KRNNvK.rtbz", 6841744},
    {"KRNPvK.rtbw", 506960},
    {"KRNPvK.rtbz", 810960},
    {"KRNvK.rtbw", 2320},
    {"KRNvK.rtbz", 295632},
    {"KRNvKB.rtbw", 1607824},
    {"KRNvKB.rtbz", 21521616},
    {"KRNvKN.rtbw", 691728},
    {"KRNvKN.rtbz", 21964816},
    {"KRNvKP.rtbw", 6367376},
    {"KRNvKP.rtbz", 9350416},
    {"KRNvKQ.rtbw", 8863888},
    {"KRNvKQ.rtbz", 6803856},
    {"KRNvKR.rtbw", 1491856},
    {"KRNvKR.rtbz", 287632},
    {"KRPPvK.rtbw", 165072},
    {"KRPPvK.rtbz", 171408},
    {"KRPvK.rtbw", 5136},
    {"KRPvK.rtbz", 12944},
    {"KRPvKB.rtbw", 9939984},
    {"KRPvKB.rtbz", 15630032},
    {"KRPvKN.rtbw", 9449680},
    {"KRPvKN.rtbz", 12376144},
    {"KRPvKP.rtbw", 4446224},
    {"KRPvKP.rtbz", 1408272},
    {"KRPvKQ.rtbw", 17606928},
    {"KRPvKQ.rtbz", 31920016},
    {"KRPvKR.rtbw", 16378576},
    {"KRPvKR.rtbz", 13725456},
    {"KRRBvK.rtbw", 159504},
    {"KRRBvK.rtbz", 2703248},
    {"KRRNvK.rtbw", 100624},
    {"KRRNvK.rtbz", 2725776},
    {"KRRPvK.rtbw", 241872},
    {"KRRPvK.rtbz", 630032},
    {"KRRRvK.rtbw", 54800},
    {"KRRRvK.rtbz", 561488},
    {"KRRvK.rtbw", 1936},
    {"KRRvK.rtbz", 53520},
    {"KRRvKB.rtbw", 351376},
    {"KRRvKB.rtbz", 4341328},
    {"KRRvKN.rtbw", 286288},
    {"KRRvKN.rtbz", 3313680},
    {"KRRvKP.rtbw", 1680272},
    {"KRRvKP.rtbz", 1885520},
    {"KRRvKQ.rtbw", 4068432},
    {"KRRvKQ.rtbz", 1127952},
    {"KRRvKR.rtbw", 334224},
    {"KRRvKR.rtbz", 7700816},
    {"KRvK.rtbw", 208},
    {"KRvK.rtbz", 7632},
    {"KRvKB.rtbw", 32912},
    {"KRvKB.rtbz", 9936},
    {"KRvKN.rtbw", 100048},
    {"KRvKN.rtbz", 93200},
    {"KRvKP.rtbw", 179408},
    {"KRvKP.rtbz", 193424},
    {"KRvKR.rtbw", 12944},
    {"KRvKR.rtbz", 3408},
}};

bool table_magic_matches(const std::filesystem::path& file) {
    static constexpr std::array<unsigned char, 4> kWdlMagic{0x71, 0xE8, 0x23, 0x5D};
    static constexpr std::array<unsigned char, 4> kDtzMagic{0xD7, 0x66, 0x0C, 0xA5};
    const std::string extension = file.extension().string();
    const auto* expected = [&]() -> const std::array<unsigned char, 4>* {
        if (extension == ".rtbw") {
            return &kWdlMagic;
        }
        if (extension == ".rtbz") {
            return &kDtzMagic;
        }
        return nullptr;
    }();
    if (expected == nullptr) {
        return false;
    }
    std::ifstream stream(file, std::ios::binary);
    std::array<unsigned char, 4> magic{};
    stream.read(reinterpret_cast<char*>(magic.data()), magic.size());
    return stream.gcount() == static_cast<std::streamsize>(magic.size()) && magic == *expected;
}

} // namespace

std::optional<std::uint64_t> expected_standard_syzygy_size(std::string_view filename) {
    for (const StandardSyzygyFile& file : kStandardSyzygyFiles) {
        if (file.name == filename) {
            return file.size;
        }
    }
    return std::nullopt;
}

SyzygyDirectoryStatus syzygy_directory_status(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return SyzygyDirectoryStatus::missing;
    }
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        return SyzygyDirectoryStatus::missing;
    }
    bool has_table = false;
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) {
            continue;
        }
        const std::string extension = entry.path().extension().string();
        if (extension != ".rtbw" && extension != ".rtbz") {
            continue;
        }
        has_table = true;
        if (!table_magic_matches(entry.path())) {
            std::ifstream probe(entry.path(), std::ios::binary);
            return probe ? SyzygyDirectoryStatus::corrupt
                         : SyzygyDirectoryStatus::unreadable;
        }
        std::error_code size_error;
        const std::uintmax_t file_size = std::filesystem::file_size(entry.path(), size_error);
        if (!size_error) {
            const auto expected =
                expected_standard_syzygy_size(entry.path().filename().string());
            if (expected.has_value() &&
                static_cast<std::uint64_t>(file_size) != *expected) {
                return SyzygyDirectoryStatus::corrupt;
            }
        }
        // A zero-filled payload that happens to match the canonical size used
        // to be handed to Fathom, where it could hang a probe.  Every real
        // Syzygy table has non-zero header/data bytes immediately after the
        // 4-byte magic; reject the all-zero body before mapping the file.
        std::ifstream payload(entry.path(), std::ios::binary);
        std::array<unsigned char, 64> payload_bytes{};
        payload.read(reinterpret_cast<char*>(payload_bytes.data()),
                     static_cast<std::streamsize>(payload_bytes.size()));
        const std::streamsize payload_count = payload.gcount();
        if (payload_count <= 4) {
            return SyzygyDirectoryStatus::corrupt;
        }
        bool payload_nonzero = false;
        for (std::streamsize i = 4; i < payload_count; ++i) {
            if (payload_bytes[static_cast<std::size_t>(i)] != 0) {
                payload_nonzero = true;
                break;
            }
        }
        if (!payload_nonzero) {
            return SyzygyDirectoryStatus::corrupt;
        }
    }
    return has_table ? SyzygyDirectoryStatus::usable : SyzygyDirectoryStatus::empty;
}

bool is_usable_syzygy_directory(const std::filesystem::path& directory) {
    return syzygy_directory_status(directory) == SyzygyDirectoryStatus::usable;
}

std::optional<std::filesystem::path> discover_syzygy_directory(
    const std::filesystem::path& executable_directory) {
    if (executable_directory.empty()) {
        return std::nullopt;
    }
    static constexpr std::array<std::string_view, 4> kCandidateNames{
        "syzygy", "syzygy345", "tablebases", "3-4-5"};
    for (const std::string_view candidate : kCandidateNames) {
        const std::filesystem::path directory = executable_directory / candidate;
        if (!is_usable_syzygy_directory(directory)) {
            continue;
        }
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(directory, error);
        if (error) {
            return directory;
        }
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
        return error ? absolute : canonical;
    }
    return std::nullopt;
}

} // namespace koi
