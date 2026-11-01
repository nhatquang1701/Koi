#pragma once

// Value types for the transposition-table memory policy and resize reporting.
//
// These are part of the SearchService API (hash configuration, resize results,
// test injection seams) but are not part of the table's storage/probe
// implementation.  Keeping them in this small header stops every consumer of
// `search_service.hpp` from pulling in the full transposition-table definition
// (hazard slots, entry layout, striped locks, ...).

#include <cstddef>
#include <cstdint>
#include <functional>

namespace koi {

enum class HashResizeStatus : std::uint8_t {
    applied,
    reduced,
    unchanged,
    disabled
};

enum class HashResizeReason : std::uint8_t {
    none,
    request_clamped,
    physical_memory_cap,
    commit_cap,
    allocation_failed,
    startup_unavailable
};

struct HashResizeResult {
    std::size_t requested_mb = 0;
    std::size_t effective_mb = 0;
    std::size_t allocated_bytes = 0;
    std::size_t segment_count = 0;
    std::uint64_t total_physical_bytes = 0;
    std::uint64_t available_physical_bytes = 0;
    std::uint64_t available_commit_bytes = 0;
    HashResizeStatus status = HashResizeStatus::unchanged;
    HashResizeReason reason = HashResizeReason::none;
    // The new storage is live, but the retired storage is still referenced by
    // a reader pin and is retained for safe deferred reclamation.
    bool reclamation_deferred = false;
};

struct HashMemorySnapshot {
    std::uint64_t total_physical_bytes = 0;
    std::uint64_t available_physical_bytes = 0;
    std::uint64_t available_commit_bytes = 0;
};

using HashMemoryProvider = std::function<HashMemorySnapshot()>;
using HashAllocationFailureProvider = std::function<bool(std::size_t)>;

struct HashMemoryPolicy {
    HashMemoryProvider memory_provider;
    HashAllocationFailureProvider allocation_failure;
};

} // namespace koi
