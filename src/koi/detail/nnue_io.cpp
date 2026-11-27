#include "koi/detail/nnue_manifest.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace koi {
namespace {

constexpr std::size_t kContainerHeaderBytes = 72;

// The largest supported container is a v5/v6 payload of 36864 inputs x 1536
// hidden int16 weights plus the L1 matrices (about 113 MiB); 256 MiB leaves
// room for a future wider hidden layer while keeping a corrupt, truncated, or
// hostile EvalFile from requesting an unbounded allocation before any
// validation runs.
constexpr std::uintmax_t kMaximumNetworkBytes = 256ull * 1024ull * 1024ull;

NnueError make_error(NnueErrorCode code, std::string message) {
    return NnueError{code, std::move(message)};
}

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

class Sha256 final {
public:
    Sha256() noexcept
        : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(std::span<const std::uint8_t> bytes) noexcept {
        for (const std::uint8_t byte : bytes) {
            buffer_[buffer_size_++] = byte;
            if (buffer_size_ == buffer_.size()) {
                transform();
                bit_count_ += 512;
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
        const std::uint64_t original_bits = bit_count_ + buffer_size_ * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56) {
            while (buffer_size_ < buffer_.size()) {
                buffer_[buffer_size_++] = 0;
            }
            transform();
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56) {
            buffer_[buffer_size_++] = 0;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(original_bits >> shift);
        }
        transform();

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
            digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
            digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
            digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
        }
        return digest;
    }

private:
    void transform() noexcept {
        static constexpr std::array<std::uint32_t, 64> kRoundConstants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4;
            schedule[index] = (static_cast<std::uint32_t>(buffer_[offset]) << 24) |
                (static_cast<std::uint32_t>(buffer_[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(buffer_[offset + 2]) << 8) |
                static_cast<std::uint32_t>(buffer_[offset + 3]);
        }
        for (std::size_t index = 16; index < schedule.size(); ++index) {
            const std::uint32_t first = schedule[index - 15];
            const std::uint32_t second = schedule[index - 2];
            const std::uint32_t sigma0 = rotate_right(first, 7) ^ rotate_right(first, 18) ^ (first >> 3);
            const std::uint32_t sigma1 = rotate_right(second, 17) ^ rotate_right(second, 19) ^ (second >> 10);
            schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const std::uint32_t sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
            const std::uint32_t sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bit_count_ = 0;
};

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept {
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.finish();
}

template <typename Integer>
void append_little_endian(std::vector<std::uint8_t>& output, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned unsigned_value = static_cast<Unsigned>(value);
    for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
        output.push_back(static_cast<std::uint8_t>(unsigned_value >> (shift * 8U)));
    }
}

template <typename Integer>
[[nodiscard]] bool read_little_endian(std::span<const std::uint8_t> bytes,
                                      std::size_t& offset, Integer& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(Integer)) {
        return false;
    }
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned assembled = 0;
    for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
        assembled |= static_cast<Unsigned>(bytes[offset++]) << (shift * 8U);
    }
    value = static_cast<Integer>(assembled);
    return true;
}

[[nodiscard]] std::optional<std::size_t> payload_size_for(const NnueManifest& manifest) noexcept {
    if (detail::is_halfka_hm_v9(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaHmV2FeatureSet) {
            return std::nullopt;
        }
        const std::size_t input_units = manifest.layer_sizes[0];
        const std::size_t hidden_units = manifest.layer_sizes[1];
        const std::size_t output_buckets = manifest.layer_sizes[2];
        return input_units * hidden_units * sizeof(std::int16_t) +
            hidden_units * sizeof(std::int32_t) +
            output_buckets * (2U * hidden_units) * sizeof(std::int8_t) +
            output_buckets * sizeof(std::int32_t);
    }
    if (detail::uses_halfka_threat_features(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaThreatV5FeatureSet) {
            return std::nullopt;
        }
        const std::size_t input_units = manifest.layer_sizes[0];
        const std::size_t hidden_units = manifest.layer_sizes[1];
        const std::size_t output_buckets = manifest.layer_sizes[2];
        const std::size_t l1_units = manifest.layer_sizes[3];
        return input_units * hidden_units * sizeof(std::int16_t) +
            hidden_units * sizeof(std::int32_t) +
            l1_units * hidden_units * sizeof(std::int8_t) +
            (detail::is_halfka_threat_v6(manifest) || detail::is_halfka_threat_v8(manifest) ?
                 l1_units * hidden_units * sizeof(std::int8_t) : std::size_t{0}) +
            l1_units * sizeof(std::int32_t) +
            output_buckets * l1_units * sizeof(std::int8_t) +
            output_buckets * sizeof(std::int32_t);
    }
    if (detail::is_halfka_king_bucket_v1(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaKingBucketV1FeatureSet) {
            return std::nullopt;
        }
        const std::size_t input_units = manifest.layer_sizes[0];
        const std::size_t hidden_units = manifest.layer_sizes[1];
        const std::size_t output_buckets = manifest.layer_sizes[2];
        const std::size_t head_inputs = detail::halfka_v4_head_inputs(manifest);
        return input_units * hidden_units * sizeof(std::int16_t) +
            hidden_units * sizeof(std::int32_t) +
            output_buckets * head_inputs * sizeof(std::int8_t) +
            output_buckets * sizeof(std::int32_t);
    }
    if (manifest.feature_set == kKoiNnueFeatureSet &&
        manifest.layer_sizes != kKoiNnueLayerSizes) {
        return std::nullopt;
    }
    if (manifest.feature_set == kKoiNnuePieceSquareKingPawnV2FeatureSet &&
        manifest.layer_sizes != kKoiNnuePieceSquareKingPawnV2LayerSizes) {
        return std::nullopt;
    }
    if (manifest.feature_set != kKoiNnueFeatureSet &&
        manifest.feature_set != kKoiNnuePieceSquareKingPawnV2FeatureSet) {
        return std::nullopt;
    }
    const std::size_t hidden_units = manifest.layer_sizes[1];
    const std::size_t bottleneck_units = manifest.layer_sizes[2];
    const std::size_t actual_input_units = manifest.layer_sizes[0];
    return actual_input_units * hidden_units * sizeof(std::int16_t) +
        hidden_units * sizeof(std::int32_t) +
        hidden_units * bottleneck_units * sizeof(std::int8_t) +
        bottleneck_units * sizeof(std::int32_t) +
        bottleneck_units * sizeof(std::int8_t) + sizeof(std::int32_t);
}

[[nodiscard]] NnueError invalid_file_size_error() {
    return make_error(NnueErrorCode::invalid_file_size, "NNUE container size does not match its manifest");
}

} // namespace

namespace detail {

std::optional<NnueError> validate_nnue_manifest(const NnueManifest& manifest) {
    if (manifest.magic != kKoiNnueMagic) {
        return make_error(NnueErrorCode::bad_magic, "NNUE container magic is not KOI-NNUE");
    }
    if (manifest.version != kKoiNnueFormatVersion &&
        manifest.version != kKoiNnuePerspectiveV3FormatVersion &&
        manifest.version != kKoiNnueHalfkaKingBucketV1FormatVersion &&
        manifest.version != kKoiNnueHalfkaThreatV5FormatVersion &&
        manifest.version != kKoiNnueHalfkaThreatV6FormatVersion &&
        manifest.version != kKoiNnueHalfkaThreatV8FormatVersion &&
        manifest.version != kKoiNnueHalfkaHmV9FormatVersion) {
        return make_error(NnueErrorCode::unsupported_version, "unsupported Koi NNUE container version");
    }
    if (is_halfka_hm_v9(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaHmV2FeatureSet) {
            return make_error(NnueErrorCode::unsupported_feature_set,
                              "NNUE v9 requires the halfka-hm-v2 feature set");
        }
        const std::uint32_t hidden_units = manifest.layer_sizes[1];
        if (manifest.layer_sizes[0] != kKoiNnueHalfkaHmV2FeatureCount ||
            manifest.layer_sizes[2] != kKoiNnueOutputBucketCount ||
            manifest.layer_sizes[3] != 1U ||
            hidden_units < kKoiNnueMinimumHiddenUnits ||
            hidden_units > kKoiNnueMaximumHiddenUnits || hidden_units % 2U != 0) {
            return make_error(
                NnueErrorCode::invalid_dimensions,
                "NNUE v9 layer manifest must be 7680 inputs, even hidden units in [32, 8192], "
                "8 output buckets, and perspective count 1");
        }
        if (manifest.quantization != kKoiNnueQuantization) {
            return make_error(NnueErrorCode::unsupported_quantization,
                              "only int16/int8 Koi NNUE quantization is supported");
        }
        return std::nullopt;
    }
    if (uses_halfka_threat_features(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaThreatV5FeatureSet) {
            return make_error(NnueErrorCode::unsupported_feature_set,
                              "NNUE v5/v6 requires the halfka-king-bucket-v1+threat-pairs-v1 feature set");
        }
        const std::uint32_t hidden_units = manifest.layer_sizes[1];
        const std::uint32_t l1_units = manifest.layer_sizes[3];
        if (manifest.layer_sizes[0] != kKoiNnueHalfkaThreatV5FeatureCount ||
            manifest.layer_sizes[2] != kKoiNnueOutputBucketCount ||
            hidden_units < kKoiNnueMinimumHiddenUnits ||
            hidden_units > kKoiNnueMaximumHiddenUnits || hidden_units % 2U != 0 ||
            l1_units < kKoiNnueMinimumL1Units || l1_units > kKoiNnueMaximumL1Units) {
            return make_error(
                NnueErrorCode::invalid_dimensions,
                "NNUE v5/v6 layer manifest must be 36864 inputs, even hidden units in [32, 8192], "
                "8 output buckets, and 8..128 L1 units");
        }
        if (manifest.quantization != kKoiNnueQuantization) {
            return make_error(NnueErrorCode::unsupported_quantization,
                              "only int16/int8 Koi NNUE quantization is supported");
        }
        return std::nullopt;
    }
    if (is_halfka_king_bucket_v1(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaKingBucketV1FeatureSet) {
            return make_error(NnueErrorCode::unsupported_feature_set,
                              "NNUE v4 requires the halfka-king-bucket-v1 feature set");
        }
        const std::uint32_t hidden_units = manifest.layer_sizes[1];
        if (manifest.layer_sizes[0] != kKoiNnueHalfkaKingBucketV1FeatureCount ||
            manifest.layer_sizes[2] != kKoiNnueOutputBucketCount ||
            (manifest.layer_sizes[3] != 0 && manifest.layer_sizes[3] != 2U) ||
            hidden_units < kKoiNnueMinimumHiddenUnits ||
            hidden_units > kKoiNnueMaximumHiddenUnits || hidden_units % 2U != 0) {
            return make_error(
                NnueErrorCode::invalid_dimensions,
                "NNUE v4 layer manifest must be 9216 inputs, even hidden units in [32, 8192], "
                "8 output buckets, and a 0 (single) or 2 (dual-perspective) head word");
        }
        if (manifest.quantization != kKoiNnueQuantization) {
            return make_error(NnueErrorCode::unsupported_quantization,
                              "only int16/int8 Koi NNUE quantization is supported");
        }
        return std::nullopt;
    }
    if (manifest.version == kKoiNnuePerspectiveV3FormatVersion &&
        manifest.feature_set != kKoiNnuePieceSquareKingPawnV2FeatureSet) {
        return make_error(NnueErrorCode::unsupported_feature_set,
                          "NNUE v3 requires the piece-square-king-pawn-v2 feature set");
    }
    if (manifest.feature_set != kKoiNnueFeatureSet &&
        manifest.feature_set != kKoiNnuePieceSquareKingPawnV2FeatureSet) {
        return make_error(NnueErrorCode::unsupported_feature_set, "unsupported Koi NNUE feature set");
    }
    const NnueLayerSizes expected = manifest.feature_set == kKoiNnueFeatureSet ?
        kKoiNnueLayerSizes : kKoiNnuePieceSquareKingPawnV2LayerSizes;
    if (manifest.layer_sizes != expected) {
        return make_error(
            NnueErrorCode::invalid_dimensions,
            manifest.feature_set == kKoiNnueFeatureSet ?
                "NNUE v1 layer dimensions must be 768, 128, 32, 1" :
                "NNUE v2 layer dimensions must be 960, 256, 32, 1");
    }
    if (manifest.quantization != kKoiNnueQuantization) {
        return make_error(NnueErrorCode::unsupported_quantization,
                          "only int16/int8 Koi NNUE quantization is supported");
    }
    return std::nullopt;
}

bool nnue_arrays_match_manifest(const NnueNetwork& network) noexcept {
    const auto& sizes = network.manifest.layer_sizes;
    if (is_halfka_hm_v9(network.manifest)) {
        return network.qa >= 1U && network.qb >= 1U &&
            network.eval_scale >= 1U && network.eval_scale <= kKoiNnueMaximumEvalScale &&
            network.feature_weights.size() ==
                static_cast<std::size_t>(sizes[0]) * sizes[1] &&
            network.hidden_bias.size() == sizes[1] &&
            network.bottleneck_weights.size() ==
                static_cast<std::size_t>(sizes[2]) * (2U * sizes[1]) &&
            network.bottleneck_bias.size() == sizes[2] &&
            network.l1_weights.empty() && network.l1_diff_weights.empty() &&
            network.l1_bias.empty() && network.output_weights.empty();
    }
    if (uses_halfka_threat_features(network.manifest)) {
        // v6 and v8 both carry the threat-input diff head; v8 only changes the
        // container version and metadata, not the array layout.
        const bool v6 = is_halfka_threat_v6(network.manifest) ||
            is_halfka_threat_v8(network.manifest);
        return network.feature_weights.size() ==
                static_cast<std::size_t>(sizes[0]) * sizes[1] &&
            network.hidden_bias.size() == sizes[1] &&
            network.l1_weights.size() == static_cast<std::size_t>(sizes[3]) * sizes[1] &&
            network.l1_diff_weights.size() ==
                (v6 ? static_cast<std::size_t>(sizes[3]) * sizes[1] : std::size_t{0}) &&
            network.l1_bias.size() == sizes[3] &&
            network.bottleneck_weights.size() == static_cast<std::size_t>(sizes[2]) * sizes[3] &&
            network.bottleneck_bias.size() == sizes[2] &&
            network.output_weights.empty();
    }
    if (is_halfka_king_bucket_v1(network.manifest)) {
        const std::size_t head_inputs = halfka_v4_head_inputs(network.manifest);
        return network.feature_weights.size() ==
                static_cast<std::size_t>(sizes[0]) * sizes[1] &&
            network.hidden_bias.size() == sizes[1] &&
            network.bottleneck_weights.size() ==
                static_cast<std::size_t>(sizes[2]) * head_inputs &&
            network.bottleneck_bias.size() == sizes[2] &&
            network.output_weights.empty();
    }
    return network.feature_weights.size() == static_cast<std::size_t>(sizes[0]) * sizes[1] &&
        network.hidden_bias.size() == sizes[1] &&
        network.bottleneck_weights.size() == static_cast<std::size_t>(sizes[1]) * sizes[2] &&
        network.bottleneck_bias.size() == sizes[2] &&
        network.output_weights.size() == sizes[2];
}

bool nnue_network_is_usable(const NnueNetwork& network) {
    return !validate_nnue_manifest(network.manifest).has_value() &&
        nnue_arrays_match_manifest(network);
}

} // namespace detail

std::expected<std::vector<std::uint8_t>, NnueError> NnueLoader::serialize(
    const NnueNetwork& network) {
    if (const auto error = detail::validate_nnue_manifest(network.manifest); error.has_value()) {
        return std::unexpected(*error);
    }
    if (network.manifest.version < kKoiNnuePerspectiveV3FormatVersion &&
        (network.hidden_shift != 0 || network.bottleneck_shift != 0 ||
         network.output_shift != 0)) {
        return std::unexpected(make_error(
            NnueErrorCode::malformed_manifest,
            "NNUE v1/v2 containers cannot carry nonzero shifts"));
    }
    if (detail::is_halfka_hm_v9(network.manifest)) {
        if (network.qa == 0 || network.qb == 0 ||
            network.eval_scale == 0 || network.eval_scale > kKoiNnueMaximumEvalScale ||
            network.hidden_shift != 0 || network.bottleneck_shift != 0 ||
            network.output_shift != 0 || network.l1_shift != 0 || network.l1_diff_shift != 0) {
            return std::unexpected(make_error(
                NnueErrorCode::malformed_manifest,
                "NNUE v9 must carry valid qa/qb/eval_scale and zero shift fields"));
        }
    } else if (detail::uses_halfka_threat_features(network.manifest)) {
        if (network.hidden_shift > kKoiNnueMaximumShift ||
            network.output_shift > kKoiNnueMaximumShift ||
            network.l1_shift > kKoiNnueMaximumShift ||
            network.l1_diff_shift > kKoiNnueMaximumShift ||
            network.bottleneck_shift != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                              "NNUE v5/v6 shift is out of range"));
        }
        // The v6 split shift packs into the byte v5 reserved, so a v5 network
        // must leave it at zero.
        if (!detail::is_halfka_threat_v6(network.manifest) &&
            !detail::is_halfka_threat_v8(network.manifest) && network.l1_diff_shift != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                              "NNUE v5 containers cannot carry a diff shift"));
        }
    } else if (detail::is_halfka_king_bucket_v1(network.manifest)) {
        if (network.hidden_shift > kKoiNnueMaximumShift ||
            network.output_shift > kKoiNnueMaximumShift ||
            network.bottleneck_shift != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                              "NNUE v4 shift is out of range"));
        }
    } else if (network.manifest.version >= kKoiNnuePerspectiveV3FormatVersion &&
               (network.hidden_shift > kKoiNnueMaximumShift ||
                network.bottleneck_shift > kKoiNnueMaximumShift ||
                network.output_shift > kKoiNnueMaximumShift)) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                          "NNUE v3 shift is out of range"));
    }
    const auto expected_payload_size = payload_size_for(network.manifest);
    if (!expected_payload_size.has_value() || !detail::nnue_arrays_match_manifest(network)) {
        return std::unexpected(make_error(
            NnueErrorCode::invalid_dimensions, "NNUE weight arrays do not match the manifest"));
    }
    if (network.manifest.feature_set.size() > std::numeric_limits<std::uint16_t>::max() ||
        network.manifest.quantization.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(make_error(
            NnueErrorCode::malformed_manifest, "NNUE feature-set name is too long"));
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(*expected_payload_size);
    for (const std::int16_t weight : network.feature_weights) {
        append_little_endian(payload, weight);
    }
    for (const std::int32_t bias : network.hidden_bias) {
        append_little_endian(payload, bias);
    }
    if (detail::uses_halfka_threat_features(network.manifest)) {
        for (const std::int8_t weight : network.l1_weights) {
            payload.push_back(static_cast<std::uint8_t>(weight));
        }
        for (const std::int8_t weight : network.l1_diff_weights) {
            payload.push_back(static_cast<std::uint8_t>(weight));
        }
        for (const std::int32_t bias : network.l1_bias) {
            append_little_endian(payload, bias);
        }
    }
    for (const std::int8_t weight : network.bottleneck_weights) {
        payload.push_back(static_cast<std::uint8_t>(weight));
    }
    for (const std::int32_t bias : network.bottleneck_bias) {
        append_little_endian(payload, bias);
    }
    if (!detail::is_halfka_king_bucket_v1(network.manifest) &&
        !detail::uses_halfka_threat_features(network.manifest) &&
        !detail::is_halfka_hm_v9(network.manifest)) {
        for (const std::int8_t weight : network.output_weights) {
            payload.push_back(static_cast<std::uint8_t>(weight));
        }
        append_little_endian(payload, network.output_bias);
    }
    if (payload.size() != *expected_payload_size) {
        return std::unexpected(invalid_file_size_error());
    }
    const auto checksum = sha256(payload);

    std::vector<std::uint8_t> container;
    container.reserve(kContainerHeaderBytes + network.manifest.quantization.size() +
                      network.manifest.feature_set.size() + payload.size());
    container.insert(container.end(), kKoiNnueMagic.begin(), kKoiNnueMagic.end());
    append_little_endian(container, network.manifest.version);
    for (const std::uint32_t layer_size : network.manifest.layer_sizes) {
        append_little_endian(container, layer_size);
    }
    if (detail::is_halfka_hm_v9(network.manifest)) {
        container.push_back(network.qa);
        container.push_back(network.qb);
        append_little_endian(container, network.eval_scale);
    } else if (detail::uses_halfka_threat_features(network.manifest)) {
        container.push_back(network.hidden_shift);
        container.push_back(network.output_shift);
        container.push_back(network.l1_shift);
        // The byte v5 reserved carries the v6 split pair/diff shift (zero
        // keeps the legacy combined head); validation pins it to zero for v5.
        container.push_back(network.l1_diff_shift);
    } else if (detail::is_halfka_king_bucket_v1(network.manifest)) {
        container.push_back(network.hidden_shift);
        container.push_back(network.output_shift);
        container.push_back(0);
        container.push_back(0);
    } else if (network.manifest.version >= kKoiNnuePerspectiveV3FormatVersion) {
        container.push_back(network.hidden_shift);
        container.push_back(network.bottleneck_shift);
        container.push_back(network.output_shift);
        container.push_back(0);
    }
    append_little_endian(container, static_cast<std::uint16_t>(network.manifest.quantization.size()));
    append_little_endian(container, static_cast<std::uint16_t>(network.manifest.feature_set.size()));
    append_little_endian(container, static_cast<std::uint64_t>(payload.size()));
    container.insert(container.end(), checksum.begin(), checksum.end());
    container.insert(container.end(), network.manifest.quantization.begin(), network.manifest.quantization.end());
    container.insert(container.end(), network.manifest.feature_set.begin(), network.manifest.feature_set.end());
    container.insert(container.end(), payload.begin(), payload.end());
    return container;
}

std::expected<NnueNetwork, NnueError> NnueLoader::load(
    const std::span<const std::uint8_t> container) {
    if (container.empty()) {
        return std::unexpected(make_error(NnueErrorCode::empty_container, "NNUE container is empty"));
    }
    if (container.size() < kContainerHeaderBytes) {
        return std::unexpected(invalid_file_size_error());
    }

    NnueManifest manifest;
    manifest.magic.assign(reinterpret_cast<const char*>(container.data()), kKoiNnueMagic.size());
    std::size_t offset = kKoiNnueMagic.size();
    if (!read_little_endian(container, offset, manifest.version)) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest, "NNUE manifest is truncated"));
    }
    for (std::uint32_t& layer_size : manifest.layer_sizes) {
        if (!read_little_endian(container, offset, layer_size)) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE layer manifest is truncated"));
        }
    }
    std::uint8_t hidden_shift = 0;
    std::uint8_t bottleneck_shift = 0;
    std::uint8_t output_shift = 0;
    std::uint8_t l1_shift = 0;
    std::uint8_t l1_diff_shift = 0;
    std::uint8_t v9_qa = 0;
    std::uint8_t v9_qb = 0;
    std::uint16_t v9_eval_scale = 0;
    if (detail::is_halfka_hm_v9(manifest)) {
        if (container.size() - offset < 4) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v9 scale metadata is truncated"));
        }
        v9_qa = container[offset++];
        v9_qb = container[offset++];
        if (!read_little_endian(container, offset, v9_eval_scale) ||
            v9_qa == 0 || v9_qb == 0 || v9_eval_scale == 0 ||
            v9_eval_scale > kKoiNnueMaximumEvalScale) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v9 qa/qb/eval_scale is invalid"));
        }
    } else if (detail::uses_halfka_threat_features(manifest)) {
        if (container.size() - offset < 4) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v5/v6 scale metadata is truncated"));
        }
        hidden_shift = container[offset++];
        output_shift = container[offset++];
        l1_shift = container[offset++];
        const std::uint8_t diff_or_reserved = container[offset++];
        if (detail::is_halfka_threat_v6(manifest) || detail::is_halfka_threat_v8(manifest)) {
            // v6 packs the split pair/diff shift into the byte v5 reserved;
            // zero keeps the legacy combined interpretation.
            l1_diff_shift = diff_or_reserved;
        } else if (diff_or_reserved != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v5 reserved header byte must be zero"));
        }
        if (hidden_shift > kKoiNnueMaximumShift || output_shift > kKoiNnueMaximumShift ||
            l1_shift > kKoiNnueMaximumShift || l1_diff_shift > kKoiNnueMaximumShift) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v5/v6 shift is out of range"));
        }
    } else if (detail::is_halfka_king_bucket_v1(manifest)) {
        if (container.size() - offset < 4) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v4 scale metadata is truncated"));
        }
        hidden_shift = container[offset++];
        output_shift = container[offset++];
        const std::uint8_t reserved_low = container[offset++];
        const std::uint8_t reserved_high = container[offset++];
        if (reserved_low != 0 || reserved_high != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v4 reserved header bytes must be zero"));
        }
        if (hidden_shift > kKoiNnueMaximumShift || output_shift > kKoiNnueMaximumShift) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v4 shift is out of range"));
        }
    } else if (manifest.version >= kKoiNnuePerspectiveV3FormatVersion) {
        if (container.size() - offset < 4) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v3 scale metadata is truncated"));
        }
        hidden_shift = container[offset++];
        bottleneck_shift = container[offset++];
        output_shift = container[offset++];
        ++offset;
        if (hidden_shift > kKoiNnueMaximumShift ||
            bottleneck_shift > kKoiNnueMaximumShift ||
            output_shift > kKoiNnueMaximumShift) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v3 shift is out of range"));
        }
    }
    std::uint16_t quantization_size = 0;
    std::uint16_t feature_set_size = 0;
    if (!read_little_endian(container, offset, quantization_size) ||
        !read_little_endian(container, offset, feature_set_size)) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest, "NNUE feature-set length is truncated"));
    }
    if (!read_little_endian(container, offset, manifest.payload_size) ||
        container.size() - offset < manifest.network_sha256.size()) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest, "NNUE checksum metadata is truncated"));
    }
    std::copy_n(container.begin() + static_cast<std::ptrdiff_t>(offset),
                manifest.network_sha256.size(), manifest.network_sha256.begin());
    offset += manifest.network_sha256.size();
    if (container.size() - offset < static_cast<std::size_t>(quantization_size) + feature_set_size) {
        return std::unexpected(invalid_file_size_error());
    }
    manifest.quantization.assign(
        reinterpret_cast<const char*>(container.data() + offset), quantization_size);
    offset += quantization_size;
    manifest.feature_set.assign(
        reinterpret_cast<const char*>(container.data() + offset), feature_set_size);
    offset += feature_set_size;

    if (const auto error = detail::validate_nnue_manifest(manifest); error.has_value()) {
        return std::unexpected(*error);
    }
    const auto expected_payload_size = payload_size_for(manifest);
    if (!expected_payload_size.has_value() || manifest.payload_size != *expected_payload_size ||
        manifest.payload_size != container.size() - offset) {
        return std::unexpected(invalid_file_size_error());
    }
    const auto payload = container.subspan(offset, static_cast<std::size_t>(manifest.payload_size));
    if (sha256(payload) != manifest.network_sha256) {
        return std::unexpected(make_error(NnueErrorCode::checksum_mismatch, "NNUE payload checksum mismatch"));
    }

    NnueNetwork network;
    network.manifest = std::move(manifest);
    network.hidden_shift = hidden_shift;
    network.bottleneck_shift = bottleneck_shift;
    network.output_shift = output_shift;
    network.l1_shift = l1_shift;
    network.l1_diff_shift = l1_diff_shift;
    network.qa = v9_qa;
    network.qb = v9_qb;
    network.eval_scale = v9_eval_scale;
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_units = network.manifest.layer_sizes[2];
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    const bool threat = detail::uses_halfka_threat_features(network.manifest);
    const bool halfka_v4 = detail::is_halfka_king_bucket_v1(network.manifest);
    const bool halfka_v9 = detail::is_halfka_hm_v9(network.manifest);
    const std::size_t feature_weight_count = input_units * hidden_units;
    std::size_t payload_offset = 0;
    network.feature_weights.resize(feature_weight_count);
    for (std::int16_t& weight : network.feature_weights) {
        if (!read_little_endian(payload, payload_offset, weight)) {
            return std::unexpected(invalid_file_size_error());
        }
    }
    network.hidden_bias.resize(hidden_units);
    for (std::int32_t& bias : network.hidden_bias) {
        std::uint32_t encoded = 0;
        if (!read_little_endian(payload, payload_offset, encoded)) {
            return std::unexpected(invalid_file_size_error());
        }
        bias = static_cast<std::int32_t>(encoded);
    }
    if (threat) {
        network.l1_weights.resize(l1_units * hidden_units);
        for (std::int8_t& weight : network.l1_weights) {
            if (payload_offset >= payload.size()) {
                return std::unexpected(invalid_file_size_error());
            }
            weight = static_cast<std::int8_t>(payload[payload_offset++]);
        }
        if (detail::is_halfka_threat_v6(network.manifest) ||
            detail::is_halfka_threat_v8(network.manifest)) {
            network.l1_diff_weights.resize(l1_units * hidden_units);
            for (std::int8_t& weight : network.l1_diff_weights) {
                if (payload_offset >= payload.size()) {
                    return std::unexpected(invalid_file_size_error());
                }
                weight = static_cast<std::int8_t>(payload[payload_offset++]);
            }
        }
        network.l1_bias.resize(l1_units);
        for (std::int32_t& bias : network.l1_bias) {
            std::uint32_t encoded = 0;
            if (!read_little_endian(payload, payload_offset, encoded)) {
                return std::unexpected(invalid_file_size_error());
            }
            bias = static_cast<std::int32_t>(encoded);
        }
    }
    const std::size_t v4_head_inputs = detail::halfka_v4_head_inputs(network.manifest);
    network.bottleneck_weights.resize(
        threat ? output_units * l1_units :
        (halfka_v9 ? output_units * (2U * hidden_units) :
        (halfka_v4 ? output_units * v4_head_inputs : hidden_units * output_units)));
    for (std::int8_t& weight : network.bottleneck_weights) {
        if (payload_offset >= payload.size()) {
            return std::unexpected(invalid_file_size_error());
        }
        weight = static_cast<std::int8_t>(payload[payload_offset++]);
    }
    network.bottleneck_bias.resize(output_units);
    for (std::int32_t& bias : network.bottleneck_bias) {
        std::uint32_t encoded = 0;
        if (!read_little_endian(payload, payload_offset, encoded)) {
            return std::unexpected(invalid_file_size_error());
        }
        bias = static_cast<std::int32_t>(encoded);
    }
    if (!halfka_v4 && !halfka_v9 && !threat) {
        network.output_weights.resize(output_units);
        for (std::int8_t& weight : network.output_weights) {
            if (payload_offset >= payload.size()) {
                return std::unexpected(invalid_file_size_error());
            }
            weight = static_cast<std::int8_t>(payload[payload_offset++]);
        }
        std::uint32_t output_bias = 0;
        if (!read_little_endian(payload, payload_offset, output_bias)) {
            return std::unexpected(invalid_file_size_error());
        }
        network.output_bias = static_cast<std::int32_t>(output_bias);
    }
    if (payload_offset != payload.size()) {
        return std::unexpected(invalid_file_size_error());
    }
    return network;
}

std::expected<NnueNetwork, NnueError> NnueLoader::load_file(const std::filesystem::path& path) {
    // Reject non-regular files before opening: opening a FIFO/named pipe for
    // reading can block indefinitely and would stall the whole UCI loop.
    std::error_code type_error;
    if (!std::filesystem::is_regular_file(path, type_error) || type_error) {
        return std::unexpected(make_error(NnueErrorCode::io_error,
                                          "NNUE container path is not a regular file"));
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::unexpected(make_error(NnueErrorCode::io_error, "unable to open NNUE container"));
    }
    const std::streampos end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(make_error(NnueErrorCode::io_error, "unable to size NNUE container"));
    }
    if (static_cast<std::uintmax_t>(end) > kMaximumNetworkBytes) {
        // Reject before allocating: a bad EvalFile path (or a sparse file) must
        // not be able to exhaust memory from inside the UCI loop.
        return std::unexpected(make_error(NnueErrorCode::io_error,
                                          "NNUE container exceeds the 256 MiB limit"));
    }
    std::vector<std::uint8_t> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(end));
    } catch (const std::exception&) {
        // Surface allocation failure as a load error so the caller can keep the
        // current evaluator instead of terminating on an escaping bad_alloc.
        return std::unexpected(make_error(NnueErrorCode::io_error,
                                          "unable to allocate memory for the NNUE container"));
    }
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(make_error(NnueErrorCode::io_error, "unable to read NNUE container"));
    }
    return load(bytes);
}

std::optional<std::filesystem::path> discover_nnue_network(
    const std::filesystem::path& directory) {
    if (directory.empty()) {
        return std::nullopt;
    }
    if (const char* disabled = std::getenv("KOI_NNUE_AUTODETECT");
        disabled != nullptr && *disabled != '\0' && std::string_view(disabled) == "0") {
        return std::nullopt;
    }
    const auto regular_file = [](const std::filesystem::path& path) {
        std::error_code error;
        const bool file = std::filesystem::is_regular_file(path, error);
        return file && !error;
    };
    const std::filesystem::path nnue_directory = directory / "nnue";
    const std::array<std::filesystem::path, 2> preferred{
        directory / "koi.nnue", nnue_directory / "koi.nnue"};
    for (const std::filesystem::path& candidate : preferred) {
        if (regular_file(candidate)) {
            return candidate;
        }
    }
    const auto single_network = [&regular_file](const std::filesystem::path& search)
        -> std::optional<std::filesystem::path> {
        std::error_code error;
        std::optional<std::filesystem::path> found;
        for (std::filesystem::directory_iterator it(search, error), end;
             !error && it != end; it.increment(error)) {
            const std::filesystem::path& path = it->path();
            if (path.extension() != ".nnue" || !regular_file(path)) {
                continue;
            }
            if (found.has_value()) {
                return std::nullopt;  // ambiguous: never guess between networks
            }
            found = path;
        }
        return found;
    };
    if (auto found = single_network(directory)) {
        return found;
    }
    return single_network(nnue_directory);
}

EvaluatorSelection make_evaluator(std::optional<std::filesystem::path> nnue_path) {
    EvaluatorSelection selection;
    // NNUE is the default boot evaluator (`KOI_EVALUATOR=hce` / `EvalMode=hce`
    // select ClassicalEvaluator separately). Missing or rejected network files
    // leave `evaluator` null so the caller reports the failure explicitly.
    if (!nnue_path.has_value()) {
        selection.nnue_error = NnueError{NnueErrorCode::malformed_manifest,
                                          "no network path given"};
        return selection;
    }

    const auto loaded = NnueLoader::load_file(*nnue_path);
    if (!loaded.has_value()) {
        selection.nnue_error = loaded.error();
        return selection;
    }
    const auto network = std::make_shared<const NnueNetwork>(std::move(*loaded));
    selection.evaluator = std::make_shared<NnueEvaluator>(network);
    selection.nnue_enabled = true;
    return selection;
}

} // namespace koi
