#include "koi/transposition_table.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define KOI_TT_PREFETCH(address) _mm_prefetch(reinterpret_cast<const char*>(address), _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
// __builtin_prefetch exists on every GCC/Clang target, including Apple
// Silicon; the backend lowers it to a hint or a no-op as the ISA allows.
#define KOI_TT_PREFETCH(address) __builtin_prefetch(address)
#else
#define KOI_TT_PREFETCH(address) ((void)(address))
#endif

namespace koi {
namespace {

constexpr int kMateThreshold = 99'000;
constexpr std::size_t kBytesPerMegabyte = 1024ULL * 1024ULL;
constexpr std::size_t kSegmentBytes = 32ULL * kBytesPerMegabyte;
constexpr std::size_t kMinimumMegabytes = 1;
constexpr std::size_t kMaximumMegabytes = 4096;
// Keep several entries in each replacement bucket.  The search key includes
// rule-history context, so unrelated reversible paths collide more often than
// a board-only TT would.  A small cluster preserves those useful bounds while
// keeping the probe/store lock and memory layout cache-friendly.
constexpr std::size_t kClusterSize = 4;
// Number of slots inspected by TranspositionTable::hashfull_permill().  The
// UCI value is permill, so 1000 slots is the natural resolution and keeps the
// sampling cost independent of the configured Hash size.
constexpr std::size_t kHashfullSampleSlots = 1000;

// Lockless 24-byte slot backed by three atomic 64-bit words.  The first word
// stores `key ^ word1 ^ word2` so a reader detects any torn concurrent write
// across the three words without taking mutexes on probe or store.
struct AtomicSlot {
    std::atomic<std::uint64_t> key_tag{0};
    std::atomic<std::uint64_t> word1{0};
    std::atomic<std::uint64_t> word2{0};
};

static_assert(sizeof(AtomicSlot) == sizeof(TranspositionEntry),
              "AtomicSlot must preserve TranspositionEntry cache footprint");

struct UnpackedSlot {
    std::uint64_t key = 0;
    std::uint64_t word1 = 0;
    std::uint64_t word2 = 0;
    int score = 0;
    Move best_move = Move::no_move();
    std::int16_t eval = kNoEvaluation;
    std::int8_t depth = 0;
    std::uint8_t generation = 0;
    std::uint8_t generation_age = 0;
    TranspositionBound bound = TranspositionBound::exact;
    bool occupied = false;
    bool pv = false;
};

[[nodiscard]] bool slot_occupied(const std::uint64_t word2) noexcept {
    return ((word2 >> 48U) & 0xFFU) != 0U;
}

[[nodiscard]] std::uint8_t slot_epoch(const std::uint64_t word2) noexcept {
    return static_cast<std::uint8_t>((word2 >> 32U) & 0xFFU);
}

[[nodiscard]] std::uint64_t pack_word1(const int stored_score, const Move best_move) noexcept {
    const std::uint32_t score_bits =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(stored_score));
    const std::uint32_t move_bits = best_move.raw();
    return static_cast<std::uint64_t>(score_bits) |
        (static_cast<std::uint64_t>(move_bits) << 32U);
}

[[nodiscard]] std::uint64_t pack_word2(const std::int16_t packed_eval,
                                       const std::int8_t packed_depth,
                                       const std::uint8_t generation,
                                       const std::uint8_t clear_epoch,
                                       const TranspositionBound bound,
                                       const bool pv) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::uint16_t>(packed_eval)) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(packed_depth)) << 16U) |
        (static_cast<std::uint64_t>(generation) << 24U) |
        (static_cast<std::uint64_t>(clear_epoch) << 32U) |
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(bound)) << 40U) |
        (std::uint64_t{1} << 48U) |
        (static_cast<std::uint64_t>(pv ? 1U : 0U) << 56U);
}

void write_slot(AtomicSlot& slot, const std::uint64_t key, const std::uint64_t w1,
                const std::uint64_t w2) noexcept {
    slot.key_tag.store(key ^ w1 ^ w2, std::memory_order_relaxed);
    slot.word1.store(w1, std::memory_order_relaxed);
    slot.word2.store(w2, std::memory_order_relaxed);
}

void clear_slot(AtomicSlot& slot) noexcept {
    slot.key_tag.store(0, std::memory_order_relaxed);
    slot.word1.store(0, std::memory_order_relaxed);
    slot.word2.store(0, std::memory_order_relaxed);
}

int score_for_storage(int score, int ply) noexcept {
    if (score >= kMateThreshold) {
        return score + ply;
    }
    if (score <= -kMateThreshold) {
        return score - ply;
    }
    return score;
}

int score_for_probe(int score, int ply) noexcept {
    if (score >= kMateThreshold) {
        return score - ply;
    }
    if (score <= -kMateThreshold) {
        return score + ply;
    }
    return score;
}

// Entry fields are narrower than the search-domain values that feed them.
// Clamp on the way in so the packed widths stay lossless for every realistic
// depth and evaluation while a stray extreme value cannot wrap around.
std::int8_t pack_depth(int depth) noexcept {
    return static_cast<std::int8_t>(std::clamp(depth, -128, 127));
}

std::int16_t pack_eval(int eval) noexcept {
    if (eval == kNoEvaluation) {
        return kNoEvaluation;
    }
    return static_cast<std::int16_t>(std::clamp(eval, -32'767, 32'767));
}

HashMemorySnapshot default_memory_snapshot() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        return HashMemorySnapshot{status.ullTotalPhys, status.ullAvailPhys, status.ullAvailPageFile};
    }
#elif defined(__linux__)
    struct sysinfo status {};
    if (::sysinfo(&status) == 0) {
        const std::uint64_t unit = status.mem_unit != 0 ? status.mem_unit : 1;
        const std::uint64_t total = static_cast<std::uint64_t>(status.totalram) * unit;
        const std::uint64_t available = static_cast<std::uint64_t>(status.freeram) * unit;
        const std::uint64_t commit = (static_cast<std::uint64_t>(status.freeram) +
                                      static_cast<std::uint64_t>(status.totalswap)) *
                                     unit;
        return HashMemorySnapshot{total, available, commit};
    }
#endif
    // macOS (and any other platform) has no snapshot provider yet; a zero
    // snapshot tells resize_locked() to skip the memory caps and honor the
    // requested Hash size.
    return {};
}

std::size_t megabytes_to_bytes(std::size_t megabytes) noexcept {
    constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (megabytes > maximum / kBytesPerMegabyte) {
        return maximum;
    }
    return megabytes * kBytesPerMegabyte;
}

std::size_t u64_to_size(std::uint64_t value) noexcept {
    const std::uint64_t maximum = std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(std::min(value, maximum));
}

// The cluster index uses multiply-shift hashing (cluster_index below), so the
// cluster count does not have to be a power of two and the allocation can
// track the requested size almost exactly instead of rounding down to the
// next power-of-two cluster count.
[[nodiscard]] std::size_t round_down_power_of_two(const std::size_t value) noexcept {
    if (value <= 1) {
        return 1;
    }
    return std::bit_floor(value);
}

[[nodiscard]] std::size_t cluster_index(std::uint64_t key, std::size_t cluster_count) noexcept {
    std::uint32_t folded = static_cast<std::uint32_t>(key) ^
        static_cast<std::uint32_t>(key >> 32U);
    folded *= 0x9E3779B9U;
    const std::uint64_t product = static_cast<std::uint64_t>(folded) *
        static_cast<std::uint64_t>(cluster_count);
    return static_cast<std::size_t>(product >> 32U);
}

// Reported size rounds to the nearest megabyte so a request that lands just
// under the boundary (24-byte entries never divide 1 MiB exactly) still
// reports the requested value.
[[nodiscard]] std::size_t bytes_to_reported_megabytes(const std::size_t bytes) noexcept {
    return (bytes + kBytesPerMegabyte / 2) / kBytesPerMegabyte;
}

[[nodiscard]] std::size_t rounded_size_mb(const std::size_t megabytes) noexcept {
    const std::size_t requested_entries = std::max<std::size_t>(
        kClusterSize, megabytes_to_bytes(megabytes) / sizeof(TranspositionEntry));
    const std::size_t clusters = std::max<std::size_t>(1, requested_entries / kClusterSize);
    const std::size_t total_entries = clusters * kClusterSize;
    return std::max<std::size_t>(
        1, bytes_to_reported_megabytes(total_entries * sizeof(TranspositionEntry)));
}

// Process-wide pool of hazard slots.  A slot index is owned by a thread for
// its whole lifetime (thread_local below), not by an individual access, so the
// acquire/release mutex is touched once per thread instead of once per node.
// The pool is intentionally leaked: thread-local destructors run at thread
// exit and may outlive a function-local static's destruction.
class HazardSlotPool {
public:
    HazardSlotPool() noexcept { words_.fill(~std::uint64_t{0}); }

    [[nodiscard]] std::size_t acquire() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t word = 0; word < words_.size(); ++word) {
            if (words_[word] == 0) {
                continue;
            }
            const std::size_t bit = static_cast<std::size_t>(std::countr_zero(words_[word]));
            words_[word] &= words_[word] - 1;
            return word * 64 + bit;
        }
        return kTranspositionHazardSlots;
    }

    void release(const std::size_t index) noexcept {
        if (index >= kTranspositionHazardSlots) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        words_[index / 64] |= std::uint64_t{1} << (index % 64);
    }

private:
    std::mutex mutex_;
    std::array<std::uint64_t, (kTranspositionHazardSlots + 63) / 64> words_{};
};

[[nodiscard]] HazardSlotPool& hazard_slot_pool() noexcept {
    static HazardSlotPool* const pool = new HazardSlotPool();
    return *pool;
}

} // namespace

std::size_t TranspositionTable::register_thread_hazard_slot() noexcept {
    struct Releaser {
        std::size_t index = kTranspositionHazardSlots;
        ~Releaser() {
            if (index < kTranspositionHazardSlots) {
                hazard_slot_pool().release(index);
            }
        }
    };
    static thread_local Releaser releaser{};
    if (releaser.index >= kTranspositionHazardSlots) {
        // The first acquisition may have failed while the pool was exhausted;
        // retry on later accesses and remember the slot in the thread-local
        // releaser so it is actually returned at thread exit.
        const std::size_t slot = hazard_slot_pool().acquire();
        if (slot < kTranspositionHazardSlots) {
            releaser.index = slot;
        }
        cached_hazard_slot_ = slot;
        return slot;
    }
    cached_hazard_slot_ = releaser.index;
    return releaser.index;
}

struct TranspositionTable::Storage {
    struct Segment {
        explicit Segment(std::size_t count, const HashAllocationFailureProvider& failure)
            : entry_count(count) {
            const std::size_t bytes = count * sizeof(TranspositionEntry);
            if (failure && failure(bytes)) {
                throw std::bad_alloc();
            }
            entries = std::make_unique<AtomicSlot[]>(count);
        }

        std::unique_ptr<AtomicSlot[]> entries;
        std::size_t entry_count = 0;
    };

    explicit Storage(std::size_t megabytes, const HashAllocationFailureProvider& failure)
        : entries_per_segment(round_down_power_of_two(std::max<std::size_t>(
              kClusterSize,
              (kSegmentBytes / sizeof(TranspositionEntry)) / kClusterSize * kClusterSize))),
          entries_per_segment_mask(entries_per_segment - 1),
          segment_shift(std::countr_zero(entries_per_segment)) {
        const std::size_t requested_bytes = megabytes_to_bytes(megabytes);
        const std::size_t requested_entries = std::max<std::size_t>(
            kClusterSize, requested_bytes / sizeof(TranspositionEntry));
        cluster_count = std::max<std::size_t>(1, requested_entries / kClusterSize);
        const std::size_t total_entries = cluster_count * kClusterSize;
        const std::size_t segment_count =
            (total_entries + entries_per_segment - 1) / entries_per_segment;
        segments.reserve(segment_count);
        segment_ptrs.reserve(segment_count);
        std::size_t remaining = total_entries;
        for (std::size_t index = 0; index < segment_count; ++index) {
            const std::size_t count = std::min(remaining, entries_per_segment);
            auto segment = std::make_unique<Segment>(count, failure);
            segment_ptrs.push_back(segment->entries.get());
            segments.push_back(std::move(segment));
            remaining -= count;
        }
        total_slot_count = total_entries;
        allocated_bytes = total_entries * sizeof(TranspositionEntry);
        size_mb = std::max<std::size_t>(1, bytes_to_reported_megabytes(allocated_bytes));
    }

    [[nodiscard]] AtomicSlot& at(std::size_t slot) noexcept {
        return segment_ptrs[slot >> segment_shift][slot & entries_per_segment_mask];
    }

    [[nodiscard]] const AtomicSlot& at(std::size_t slot) const noexcept {
        return segment_ptrs[slot >> segment_shift][slot & entries_per_segment_mask];
    }

    // Base entry of a cluster.  `entries_per_segment` is a multiple of
    // `kClusterSize`, so a cluster never straddles a segment and this single
    // resolution covers all kClusterSize entries; the probe/store hot loops
    // call it once instead of once per entry through at().
    [[nodiscard]] AtomicSlot* cluster_base(std::size_t cluster) noexcept {
        return &at(cluster * kClusterSize);
    }

    [[nodiscard]] const AtomicSlot* cluster_base(std::size_t cluster) const noexcept {
        return &at(cluster * kClusterSize);
    }

    std::vector<std::unique_ptr<Segment>> segments;
    std::vector<AtomicSlot*> segment_ptrs;
    const std::size_t entries_per_segment;
    const std::size_t entries_per_segment_mask;
    const std::size_t segment_shift;
    std::size_t allocated_bytes = 0;
    std::size_t total_slot_count = 0;
    std::size_t cluster_count = 0;
    std::size_t size_mb = 0;
    std::atomic<std::uint8_t> generation{1};
    // `clear_epoch` invalidates entries logically on Clear Hash without sweeping
    // multi-gigabyte tables except on 8-bit wraparound.
    std::atomic<std::uint8_t> clear_epoch{0};
};

TranspositionTable::TranspositionTable(std::size_t megabytes, HashMemoryPolicy policy)
    : memory_policy_(std::move(policy)) {
    std::lock_guard lock(maintenance_mutex_);
    (void)resize_locked(megabytes);
}

#ifndef NDEBUG
void TranspositionTable::claim_exclusive_session() noexcept {
    // At most one exclusive session may exist per table: the compare-exchange
    // fails, and the assert fires, when another thread already holds the
    // claim.  Relaxed ordering is enough for this once-per-search debug check.
    std::thread::id expected{};
    const bool claimed = exclusive_session_owner_.compare_exchange_strong(
        expected, std::this_thread::get_id(), std::memory_order_relaxed);
    assert(claimed && "at most one exclusive TT session may exist at a time");
    (void)claimed;
}

void TranspositionTable::release_exclusive_session() noexcept {
    exclusive_session_owner_.store(std::thread::id{}, std::memory_order_relaxed);
}

void TranspositionTable::assert_exclusive_session_owner() const noexcept {
    assert(exclusive_session_owner_.load(std::memory_order_relaxed) ==
               std::this_thread::get_id() &&
           "exclusive TT access must run on the session's claiming thread");
}
#endif

void TranspositionTable::Lease::adopt_snapshot(const TranspositionTable& table) noexcept {
    std::lock_guard<std::mutex> lock(table.maintenance_mutex_);
    fallback_ = table.storage_;
    storage_ = fallback_.get();
}

std::size_t TranspositionTable::normalized_size_mb(std::size_t megabytes) noexcept {
    return std::clamp(megabytes, kMinimumMegabytes, kMaximumMegabytes);
}

HashResizeResult TranspositionTable::set_size_mb(std::size_t megabytes) noexcept {
    std::shared_ptr<Storage> retired;
    HashResizeResult result;
    {
        std::lock_guard lock(maintenance_mutex_);
        reap_deferred_storage_locked();
        result = resize_locked(megabytes, &retired);
    }
    // Wait for readers of the retired storage *outside* the maintenance lock.
    // A search thread that owns a session pin reports info/hashfull from its own
    // callback; if the resizer held the lock while spinning on that pin, the
    // callback would block on the same mutex and the resize would deadlock.
    if (retired != nullptr) {
        for (const HazardSlot& hazard : hazards_) {
            while (hazard.storage.load(std::memory_order_seq_cst) == retired.get()) {
                std::this_thread::yield();
            }
        }
    }
    return result;
}

void TranspositionTable::reap_deferred_storage_locked() noexcept {
    for (auto entry = deferred_retired_.begin(); entry != deferred_retired_.end();) {
        Storage* const candidate = entry->get();
        bool referenced = false;
        for (const HazardSlot& hazard : hazards_) {
            if (hazard.storage.load(std::memory_order_seq_cst) == candidate) {
                referenced = true;
                break;
            }
        }
        if (referenced) {
            ++entry;
        } else {
            entry = deferred_retired_.erase(entry);
        }
    }
}

HashResizeResult TranspositionTable::try_set_size_mb(std::size_t megabytes) noexcept {
    std::shared_ptr<Storage> retired;
    HashResizeResult result;
    {
        std::lock_guard lock(maintenance_mutex_);
        reap_deferred_storage_locked();
        result = resize_locked(megabytes, &retired);
    }
    if (retired != nullptr) {
        bool referenced = false;
        for (const HazardSlot& hazard : hazards_) {
            if (hazard.storage.load(std::memory_order_seq_cst) == retired.get()) {
                referenced = true;
                break;
            }
        }
        if (referenced) {
            // A reader still pins the old storage.  Keep the shared_ptr alive;
            // readers only dereference after revalidating the live pointer, so
            // a stale slot is safe, and reclamation waits for it to drain.
            std::lock_guard lock(maintenance_mutex_);
            deferred_retired_.push_back(std::move(retired));
            result.reclamation_deferred = true;
        }
    }
    return result;
}

HashResizeResult TranspositionTable::resize_locked(std::size_t megabytes,
                                                   std::shared_ptr<Storage>* retired_out) noexcept {
    HashResizeResult result;
    result.requested_mb = megabytes;
    const std::size_t normalized = normalized_size_mb(megabytes);
    const bool request_was_clamped = normalized != megabytes;
    std::shared_ptr<Storage> current = storage_;
    const std::size_t current_bytes = current == nullptr ? 0 : current->allocated_bytes;
    const std::size_t requested_bytes = megabytes_to_bytes(normalized);
    std::size_t allowed_bytes = requested_bytes;

    HashMemorySnapshot memory;
    try {
        memory = memory_policy_.memory_provider ? memory_policy_.memory_provider() :
            default_memory_snapshot();
    } catch (...) {
        memory = {};
    }
    result.total_physical_bytes = memory.total_physical_bytes;
    result.available_physical_bytes = memory.available_physical_bytes;
    result.available_commit_bytes = memory.available_commit_bytes;

    if (memory.total_physical_bytes != 0) {
        const std::size_t physical_cap = u64_to_size(memory.total_physical_bytes / 4ULL);
        if (physical_cap < allowed_bytes) {
            allowed_bytes = physical_cap;
            result.reason = HashResizeReason::physical_memory_cap;
        }
    }
    if (memory.available_commit_bytes != 0) {
        const std::size_t available_commit = u64_to_size(memory.available_commit_bytes);
        const std::size_t available_after_old = available_commit > current_bytes ?
            available_commit - current_bytes : 0;
        const std::size_t commit_cap = available_after_old / 2;
        if (commit_cap < allowed_bytes) {
            allowed_bytes = commit_cap;
            result.reason = HashResizeReason::commit_cap;
        }
    }

    if (allowed_bytes < kBytesPerMegabyte) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        if (result.reason == HashResizeReason::none) {
            result.reason = current == nullptr ? HashResizeReason::startup_unavailable :
                HashResizeReason::commit_cap;
        }
        return result;
    }

    // The allocation tracks the requested size (multiply-shift cluster
    // indexing); the reported size matches the storage.
    const std::size_t candidate_mb = rounded_size_mb(
        std::max<std::size_t>(1, bytes_to_reported_megabytes(allowed_bytes)));
    if (current != nullptr && current->size_mb == candidate_mb) {
        result.effective_mb = current->size_mb;
        result.allocated_bytes = current->allocated_bytes;
        result.segment_count = current->segments.size();
        result.status = candidate_mb < normalized || request_was_clamped ?
            HashResizeStatus::reduced : HashResizeStatus::unchanged;
        if (result.reason == HashResizeReason::none && request_was_clamped) {
            result.reason = HashResizeReason::request_clamped;
        }
        return result;
    }

    try {
        auto replacement = std::make_shared<Storage>(candidate_mb, memory_policy_.allocation_failure);
        result.effective_mb = replacement->size_mb;
        result.allocated_bytes = replacement->allocated_bytes;
        result.segment_count = replacement->segments.size();
        result.status = candidate_mb < normalized || request_was_clamped ?
            HashResizeStatus::reduced : HashResizeStatus::applied;
        if (result.reason == HashResizeReason::none && request_was_clamped) {
            result.reason = HashResizeReason::request_clamped;
        }
        Storage* const raw = replacement.get();
        try {
            // Publish the replacement first so no new reader can lease the
            // retired storage, then wait for every reader that already leased
            // it.  The local `current` reference keeps the retired storage
            // alive across the wait; afterwards the last owner drops it (unless
            // a cold-path reader holds its own snapshot), so a descheduled
            // reader can no longer keep a freed storage alive the way the old
            // "keep the last two storages" cap could.
            storage_ = std::move(replacement);
            hot_storage_.store(raw, std::memory_order_seq_cst);
            // The caller owns the wait (outside the maintenance lock) so it can
            // keep the retired storage alive without stalling hashfull/size
            // readers on the maintenance mutex.
            if (retired_out != nullptr) {
                *retired_out = std::move(current);
            }
        } catch (...) {
            result.status = current == nullptr ? HashResizeStatus::disabled :
                HashResizeStatus::unchanged;
            result.reason = HashResizeReason::allocation_failed;
            return result;
        }
        return result;
    } catch (const std::bad_alloc&) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        result.reason = HashResizeReason::allocation_failed;
        return result;
    } catch (const std::length_error&) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        result.reason = HashResizeReason::allocation_failed;
        return result;
    } catch (...) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        result.reason = HashResizeReason::allocation_failed;
        return result;
    }
}

std::size_t TranspositionTable::size_mb() const noexcept {
    std::lock_guard lock(maintenance_mutex_);
    return storage_ == nullptr ? 0 : storage_->size_mb;
}

std::size_t TranspositionTable::hashfull_permill() const noexcept {
    std::shared_ptr<Storage> storage;
    {
        std::lock_guard lock(maintenance_mutex_);
        storage = storage_;
    }
    if (storage == nullptr || storage->total_slot_count == 0) {
        return 0;
    }

    const std::uint8_t epoch = storage->clear_epoch.load(std::memory_order_relaxed);
    const std::size_t sample_slots = std::min(kHashfullSampleSlots, storage->total_slot_count);
    std::size_t used = 0;
    for (std::size_t sampled = 0; sampled < sample_slots; ++sampled) {
        // Evenly spaced samples cover the whole allocation; the old
        // stride-and-stop loop never reached the tail of small tables.
        const std::size_t slot = sampled * storage->total_slot_count / sample_slots;
        const std::uint64_t w2 = storage->at(slot).word2.load(std::memory_order_relaxed);
        if (slot_occupied(w2) && slot_epoch(w2) == epoch) {
            ++used;
        }
    }
    return sample_slots == 0 ? 0 : used * 1000 / sample_slots;
}

void TranspositionTable::clear() noexcept {
    std::lock_guard maintenance_lock(maintenance_mutex_);
    reap_deferred_storage_locked();
    const std::shared_ptr<Storage> storage = storage_;
    if (storage == nullptr || storage->total_slot_count == 0) {
        return;
    }

    const std::uint8_t epoch = storage->clear_epoch.load(std::memory_order_relaxed);
    if (epoch == std::numeric_limits<std::uint8_t>::max()) {
        storage->clear_epoch.store(0, std::memory_order_relaxed);
        for (const auto& segment : storage->segments) {
            for (std::size_t index = 0; index < segment->entry_count; ++index) {
                clear_slot(segment->entries[index]);
            }
        }
    } else {
        storage->clear_epoch.store(static_cast<std::uint8_t>(epoch + 1U),
                                   std::memory_order_relaxed);
    }
}

void TranspositionTable::new_generation() noexcept {
    // Advance the per-search replacement epoch.  This counter is internal to
    // the table; it is neither the UCI protocol generation nor a search
    // request identity (see transposition_table.hpp).
    std::lock_guard maintenance_lock(maintenance_mutex_);
    reap_deferred_storage_locked();
    const std::shared_ptr<Storage> storage = storage_;
    if (storage == nullptr || storage->total_slot_count == 0) {
        return;
    }

    const std::uint8_t gen = storage->generation.load(std::memory_order_relaxed);
    if (gen == std::numeric_limits<std::uint8_t>::max()) {
        storage->generation.store(1, std::memory_order_relaxed);
        for (const auto& segment : storage->segments) {
            for (std::size_t index = 0; index < segment->entry_count; ++index) {
                clear_slot(segment->entries[index]);
            }
        }
    } else {
        storage->generation.store(static_cast<std::uint8_t>(gen + 1U),
                                  std::memory_order_relaxed);
    }
}

void TranspositionTable::store_storage(Storage* storage, std::uint64_t key, int depth, int score,
                                       TranspositionBound bound, Move best_move, int ply, bool pv,
                                       int eval) noexcept {
    if (storage == nullptr || storage->cluster_count == 0) {
        return;
    }

    const std::uint8_t gen = storage->generation.load(std::memory_order_relaxed);
    const std::uint8_t epoch = storage->clear_epoch.load(std::memory_order_relaxed);
    const std::size_t cluster = cluster_index(key, storage->cluster_count);
    AtomicSlot* const cluster_entries = storage->cluster_base(cluster);

    const std::size_t offset_start = static_cast<std::size_t>(key) & (kClusterSize - 1U);
    std::size_t empty_offset = kClusterSize;
    std::size_t replacement_offset = 0;
    std::uint64_t replacement_key = 0;
    std::int8_t replacement_depth = 0;
    std::uint8_t replacement_generation = 0;
    std::uint32_t replacement_priority = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t step = 0; step < kClusterSize; ++step) {
        const std::size_t offset = (offset_start + step) & (kClusterSize - 1U);
        const AtomicSlot& slot = cluster_entries[offset];
        const std::uint64_t w2 = slot.word2.load(std::memory_order_relaxed);
        const bool valid = slot_occupied(w2) && slot_epoch(w2) == epoch;
        if (!valid) {
            if (empty_offset == kClusterSize) {
                empty_offset = offset;
            }
            continue;
        }
        const std::uint64_t tag = slot.key_tag.load(std::memory_order_relaxed);
        const std::uint64_t w1 = slot.word1.load(std::memory_order_relaxed);
        const std::uint64_t slot_key = tag ^ w1 ^ w2;
        if (slot_key == key) {
            const std::int8_t slot_depth =
                static_cast<std::int8_t>(static_cast<std::uint8_t>(w2 >> 16U));
            const std::uint8_t bound_raw = static_cast<std::uint8_t>(w2 >> 40U);
            const TranspositionBound slot_bound =
                bound_raw <= static_cast<std::uint8_t>(TranspositionBound::upper) ?
                    static_cast<TranspositionBound>(bound_raw) :
                    TranspositionBound::exact;
            const bool keeps_deeper_entry = slot_depth > depth;
            const bool keeps_equal_exact_entry = slot_depth == depth &&
                slot_bound == TranspositionBound::exact &&
                bound != TranspositionBound::exact;
            if (keeps_deeper_entry || keeps_equal_exact_entry) {
                const std::uint8_t slot_gen = static_cast<std::uint8_t>(w2 >> 24U);
                if (slot_gen != gen) {
                    const std::int16_t slot_eval = static_cast<std::int16_t>(
                        static_cast<std::uint16_t>(w2));
                    const bool slot_pv = ((w2 >> 56U) & 0xFFU) != 0U;
                    const std::uint64_t refreshed_w2 = pack_word2(
                        slot_eval, slot_depth, gen, epoch, slot_bound, slot_pv);
                    write_slot(cluster_entries[offset], key, w1, refreshed_w2);
                }
                return;
            }
            const std::uint64_t new_w1 = pack_word1(score_for_storage(score, ply), best_move);
            const std::uint64_t new_w2 = pack_word2(
                pack_eval(eval), pack_depth(depth), gen, epoch, bound, pv);
            write_slot(cluster_entries[offset], key, new_w1, new_w2);
            return;
        }

        if (empty_offset != kClusterSize) {
            continue;
        }
        // Prefer stale generations, then shallow entries.  The final key
        // component makes replacement deterministic when all other signals tie.
        const std::uint8_t slot_gen = static_cast<std::uint8_t>(w2 >> 24U);
        const std::int8_t slot_depth =
            static_cast<std::int8_t>(static_cast<std::uint8_t>(w2 >> 16U));
        const std::uint32_t age_rank = (slot_gen == gen) ? 1U : 0U;
        const std::uint32_t depth_rank =
            slot_depth > 0 ? static_cast<std::uint32_t>(slot_depth) : 0U;
        const std::uint32_t priority = (age_rank << 24U) |
            (depth_rank << 8U) |
            (static_cast<std::uint32_t>(slot_key) & 0xFFU);
        if (priority < replacement_priority) {
            replacement_priority = priority;
            replacement_offset = offset;
            replacement_generation = slot_gen;
            replacement_depth = slot_depth;
            replacement_key = slot_key;
        }
    }

    const std::size_t target_offset = empty_offset != kClusterSize ?
        empty_offset : replacement_offset;
    if (empty_offset == kClusterSize) {
        const bool older_generation = replacement_generation != gen;
        const bool deeper = depth > replacement_depth;
        const bool deterministic_tie_break =
            depth == replacement_depth && key < replacement_key;
        if (!older_generation && !deeper && !deterministic_tie_break) {
            return;
        }
    }
    const std::uint64_t new_w1 = pack_word1(score_for_storage(score, ply), best_move);
    const std::uint64_t new_w2 = pack_word2(
        pack_eval(eval), pack_depth(depth), gen, epoch, bound, pv);
    write_slot(cluster_entries[target_offset], key, new_w1, new_w2);
}

void TranspositionTable::store_exclusive(Storage* storage, std::uint64_t key, int depth,
                                         int score, TranspositionBound bound, Move best_move,
                                         int ply, bool pv, int eval) noexcept {
#ifndef NDEBUG
    assert_exclusive_session_owner();
#endif
    store_storage(storage, key, depth, score, bound, best_move, ply, pv, eval);
}

void TranspositionTable::store(std::uint64_t key, int depth, int score, TranspositionBound bound,
                               Move best_move, int ply, bool pv, int eval,
                               TranspositionAccessMode mode) noexcept {
#ifndef NDEBUG
    if (mode == TranspositionAccessMode::exclusive) {
        assert_exclusive_session_owner();
    }
#else
    (void)mode;
#endif
    const Lease lease(*this);
    store_storage(lease.storage(), key, depth, score, bound, best_move, ply, pv, eval);
}

std::optional<TranspositionEntry> TranspositionTable::probe_storage(
    const Storage* storage, std::uint64_t key, int ply) noexcept {
    if (storage == nullptr || storage->cluster_count == 0) {
        return std::nullopt;
    }

    const std::uint8_t epoch = storage->clear_epoch.load(std::memory_order_relaxed);
    const std::uint8_t gen = storage->generation.load(std::memory_order_relaxed);
    const std::size_t cluster = cluster_index(key, storage->cluster_count);
    const AtomicSlot* const cluster_entries = storage->cluster_base(cluster);

    for (std::size_t offset = 0; offset < kClusterSize; ++offset) {
        const AtomicSlot& slot = cluster_entries[offset];
        const std::uint64_t tag = slot.key_tag.load(std::memory_order_relaxed);
        const std::uint64_t w1 = slot.word1.load(std::memory_order_relaxed);
        const std::uint64_t w2 = slot.word2.load(std::memory_order_relaxed);
        if ((tag ^ w1 ^ w2) != key || !slot_occupied(w2) || slot_epoch(w2) != epoch) {
            continue;
        }
        TranspositionEntry result;
        result.key = key;
        const int raw_score = static_cast<int>(
            static_cast<std::int32_t>(static_cast<std::uint32_t>(w1 & 0xFFFFFFFFULL)));
        result.score = score_for_probe(raw_score, ply);
        result.best_move = Move::from_raw(static_cast<std::uint32_t>(w1 >> 32U));
        result.eval = static_cast<std::int16_t>(static_cast<std::uint16_t>(w2 & 0xFFFFULL));
        result.depth = static_cast<std::int8_t>(static_cast<std::uint8_t>((w2 >> 16U) & 0xFFU));
        const std::uint8_t slot_gen = static_cast<std::uint8_t>((w2 >> 24U) & 0xFFU);
        result.generation = slot_gen;
        result.generation_age = static_cast<std::uint8_t>(gen - slot_gen);
        const std::uint8_t bound_raw = static_cast<std::uint8_t>((w2 >> 40U) & 0xFFU);
        result.bound = bound_raw <= static_cast<std::uint8_t>(TranspositionBound::upper) ?
            static_cast<TranspositionBound>(bound_raw) :
            TranspositionBound::exact;
        result.occupied = true;
        result.pv = ((w2 >> 56U) & 0xFFU) != 0U;
        return result;
    }
    return std::nullopt;
}

std::optional<TranspositionEntry> TranspositionTable::probe_exclusive(
    Storage* storage, std::uint64_t key, int ply) const noexcept {
#ifndef NDEBUG
    assert_exclusive_session_owner();
#endif
    return probe_storage(storage, key, ply);
}

std::optional<TranspositionEntry> TranspositionTable::probe(
    std::uint64_t key, int ply, TranspositionAccessMode mode) const noexcept {
#ifndef NDEBUG
    if (mode == TranspositionAccessMode::exclusive) {
        assert_exclusive_session_owner();
    }
#else
    (void)mode;
#endif
    const Lease lease(*this);
    return probe_storage(lease.storage(), key, ply);
}

void TranspositionTable::prefetch_storage(const Storage* storage, std::uint64_t key) noexcept {
    if (storage == nullptr || storage->cluster_count == 0) {
        return;
    }
    const std::size_t cluster = cluster_index(key, storage->cluster_count);
    KOI_TT_PREFETCH(storage->cluster_base(cluster));
}

void TranspositionTable::prefetch(std::uint64_t key) const noexcept {
    // Resolve the address under the same hazard lease as the probe: a resize
    // may run during a concurrent search, so a lease-free read could touch
    // retired storage.
    const Lease lease(*this);
    prefetch_storage(lease.storage(), key);
}

void TranspositionTable::prefetch_exclusive(Storage* storage, std::uint64_t key) const noexcept {
    prefetch_storage(storage, key);
}

} // namespace koi
