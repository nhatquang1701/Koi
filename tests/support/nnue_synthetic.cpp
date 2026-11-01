// Test-only synthetic NNUE fixtures.
//
// These factories used to live in the shipped engine (src/koi/nnue.cpp) even
// though only tests and the koi-nnue-fixture development tool use them.  They
// are defined here so the engine library contains no synthetic-network code;
// the member declarations remain in nnue.hpp for source compatibility.

#include <cstddef>
#include <cstdint>
#include <string>

#include "koi/nnue.hpp"

namespace koi::test {

NnueNetwork synthetic_network() {
    NnueNetwork network;
    network.manifest.magic = std::string(kKoiNnueMagic);
    network.manifest.version = kKoiNnueFormatVersion;
    network.manifest.layer_sizes = kKoiNnueLayerSizes;
    network.manifest.feature_set = std::string(kKoiNnueFeatureSet);
    network.manifest.quantization = std::string(kKoiNnueQuantization);
    const std::size_t input_units = kKoiNnueLayerSizes[0];
    const std::size_t hidden_units = kKoiNnueLayerSizes[1];
    const std::size_t bottleneck_units = kKoiNnueLayerSizes[2];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.bottleneck_weights.resize(hidden_units * bottleneck_units);
    network.bottleneck_bias.resize(bottleneck_units);
    network.output_weights.resize(bottleneck_units);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] = static_cast<std::int16_t>(static_cast<int>(index % 5U) - 2);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index) - 1;
        network.output_weights[index] = static_cast<std::int8_t>((index % 3U) + 1U);
    }
    return network;
}

NnueNetwork synthetic_network_v2() {
    NnueNetwork network = synthetic_network();
    network.manifest.layer_sizes = kKoiNnuePieceSquareKingPawnV2LayerSizes;
    network.manifest.feature_set = std::string(kKoiNnuePieceSquareKingPawnV2FeatureSet);
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.bottleneck_weights.resize(hidden_units * bottleneck_units);
    network.bottleneck_bias.resize(bottleneck_units);
    network.output_weights.resize(bottleneck_units);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] = static_cast<std::int16_t>(static_cast<int>(index % 5U) - 2);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index) - 1;
        network.output_weights[index] = static_cast<std::int8_t>((index % 3U) + 1U);
    }
    return network;
}

NnueNetwork synthetic_network_v4() {
    NnueNetwork network;
    network.manifest.magic = std::string(kKoiNnueMagic);
    network.manifest.version = kKoiNnueHalfkaKingBucketV1FormatVersion;
    network.manifest.layer_sizes = {
        kKoiNnueHalfkaKingBucketV1FeatureCount, 32, kKoiNnueOutputBucketCount, 0};
    network.manifest.feature_set = std::string(kKoiNnueHalfkaKingBucketV1FeatureSet);
    network.manifest.quantization = std::string(kKoiNnueQuantization);
    network.hidden_shift = 7;
    network.output_shift = 15;
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_buckets = network.manifest.layer_sizes[2];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.bottleneck_weights.resize(output_buckets * (hidden_units / 2U));
    network.bottleneck_bias.resize(output_buckets);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] = static_cast<std::int16_t>(static_cast<int>(index % 5U) - 2);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    return network;
}

NnueNetwork synthetic_network_v5() {
    NnueNetwork network;
    network.manifest.magic = std::string(kKoiNnueMagic);
    network.manifest.version = kKoiNnueHalfkaThreatV5FormatVersion;
    network.manifest.layer_sizes = {
        kKoiNnueHalfkaThreatV5FeatureCount, 32, kKoiNnueOutputBucketCount, 8};
    network.manifest.feature_set = std::string(kKoiNnueHalfkaThreatV5FeatureSet);
    network.manifest.quantization = std::string(kKoiNnueQuantization);
    network.hidden_shift = 7;
    network.l1_shift = 6;
    network.output_shift = 12;
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_buckets = network.manifest.layer_sizes[2];
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.l1_weights.resize(l1_units * hidden_units);
    network.l1_bias.resize(l1_units);
    network.bottleneck_weights.resize(output_buckets * l1_units);
    network.bottleneck_bias.resize(output_buckets);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] = static_cast<std::int16_t>(static_cast<int>(index % 5U) - 2);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.l1_weights.size(); ++index) {
        network.l1_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.l1_bias.size(); ++index) {
        network.l1_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    return network;
}

NnueNetwork synthetic_network_v6() {
    NnueNetwork network = synthetic_network_v5();
    network.manifest.version = kKoiNnueHalfkaThreatV6FormatVersion;
    network.l1_diff_weights.resize(network.l1_weights.size());
    for (std::size_t index = 0; index < network.l1_diff_weights.size(); ++index) {
        network.l1_diff_weights[index] =
            static_cast<std::int8_t>((index % 2U == 0U) ? 1 : -1);
    }
    return network;
}

} // namespace koi::test
