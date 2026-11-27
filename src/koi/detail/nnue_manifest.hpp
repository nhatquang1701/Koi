#pragma once

#include <cstddef>
#include <optional>

#include "koi/nnue.hpp"

namespace koi::detail {

// True for every container that uses the halfka-only layout: the historical v4
// container and, from 1.1.4, the version-8 container.  Version 8 carries either
// layout, so the input width decides: 9216 inputs is the halfka-only form, and
// `is_halfka_threat_v8` claims the 36864-input threat form.  Both predicates are
// used while parsing the header, before the feature-set string is available, so
// they must only consult the version and the layer sizes.
[[nodiscard]] constexpr bool is_halfka_king_bucket_v1(
    const NnueManifest& manifest) noexcept {
    if (manifest.version == kKoiNnueHalfkaKingBucketV1FormatVersion) {
        return true;
    }
    return manifest.version == kKoiNnueHalfkaThreatV8FormatVersion &&
        manifest.layer_sizes[0] == kKoiNnueHalfkaKingBucketV1FeatureCount;
}

// Dual-perspective HalfKA v4 (`layer_sizes[3] == 2`): concatenates the
// side-to-move's `hidden / 2` CReLU pair products and the opponent's
// `hidden / 2` CReLU pair products into a `hidden`-wide vector feeding the
// 8 piece-count output buckets.
[[nodiscard]] constexpr bool is_halfka_dual_v4(
    const NnueManifest& manifest) noexcept {
    return is_halfka_king_bucket_v1(manifest) && manifest.layer_sizes[3] == 2U;
}

[[nodiscard]] constexpr std::size_t halfka_v4_head_inputs(
    const NnueManifest& manifest) noexcept {
    const std::size_t hidden_units = manifest.layer_sizes[1];
    return is_halfka_dual_v4(manifest) ? hidden_units : (hidden_units / 2U);
}

[[nodiscard]] constexpr bool is_halfka_threat_v5(
    const NnueManifest& manifest) noexcept {
    return manifest.version == kKoiNnueHalfkaThreatV5FormatVersion;
}

[[nodiscard]] constexpr bool is_halfka_threat_v6(
    const NnueManifest& manifest) noexcept {
    return manifest.version == kKoiNnueHalfkaThreatV6FormatVersion;
}

// The version-8 container carries either layout; only the 36864-input form has
// the threat-pair inputs and the antisymmetric diff head.
[[nodiscard]] constexpr bool is_halfka_threat_v8(
    const NnueManifest& manifest) noexcept {
    return manifest.version == kKoiNnueHalfkaThreatV8FormatVersion &&
        manifest.layer_sizes[0] == kKoiNnueHalfkaThreatV5FeatureCount;
}

[[nodiscard]] constexpr bool is_halfka_hm_v9(
    const NnueManifest& manifest) noexcept {
    return manifest.version == kKoiNnueHalfkaHmV9FormatVersion;
}

// Versions 5 and 6 share the feature set, the header skeleton, and the
// incremental path; only the L1 stage and the payload size differ.
[[nodiscard]] constexpr bool uses_halfka_threat_features(
    const NnueManifest& manifest) noexcept {
    return is_halfka_threat_v5(manifest) || is_halfka_threat_v6(manifest) ||
        is_halfka_threat_v8(manifest);
}

[[nodiscard]] std::optional<NnueError> validate_nnue_manifest(const NnueManifest& manifest);
[[nodiscard]] bool nnue_arrays_match_manifest(const NnueNetwork& network) noexcept;
[[nodiscard]] bool nnue_network_is_usable(const NnueNetwork& network);

} // namespace koi::detail
