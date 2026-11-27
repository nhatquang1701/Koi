#pragma once

// Stateless NNUE inference, activation, and incremental accumulator SIMD/scalar
// kernels separated from container I/O (detail/nnue_io.cpp) and evaluator
// worker/cache orchestration (nnue.cpp).

#include "koi/nnue.hpp"
#include "koi/detail/nnue_manifest.hpp"
#include "koi/cpu_features.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

// The AVX2 kernel exists only on x86-64; on arm64 (Apple Silicon, ARM Linux)
// NEON kernels handle v4 incremental deltas and hidden-layer inference, with
// scalar fallback on other targets or non-v4 formats.
#if (defined(_M_AVX2) || defined(__AVX2__)) && \
    (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define KOI_NNUE_COMPILED_AVX2 1
#else
#define KOI_NNUE_COMPILED_AVX2 0
#endif

// NEON kernels for arm64 (Apple Silicon, ARM Linux): v4 fused int16 accumulator
// deltas, CReLU clamp/pack, and hidden-layer inference (scalar fallback
// otherwise).
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && (defined(__aarch64__) || defined(__arm__))
#include <arm_neon.h>
#define KOI_NNUE_COMPILED_NEON 1
#else
#define KOI_NNUE_COMPILED_NEON 0
#endif

namespace koi::detail {

[[nodiscard]] inline bool arrays_match_manifest(const NnueNetwork& network) noexcept {
    return nnue_arrays_match_manifest(network);
}

[[nodiscard]] inline int clamp_score(std::int64_t score) noexcept {
    return static_cast<int>(std::clamp<std::int64_t>(
        score, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

[[nodiscard]] inline bool compute_int16_accumulator_safe(const NnueNetwork& network) noexcept {
    if (uses_halfka_threat_features(network.manifest) ||
        network.hidden_bias.empty() || network.feature_weights.empty()) {
        return false;
    }
    std::int64_t min_bias = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_bias = std::numeric_limits<std::int64_t>::min();
    for (const std::int32_t bias : network.hidden_bias) {
        min_bias = std::min<std::int64_t>(min_bias, bias);
        max_bias = std::max<std::int64_t>(max_bias, bias);
    }
    std::int16_t min_w = std::numeric_limits<std::int16_t>::max();
    std::int16_t max_w = std::numeric_limits<std::int16_t>::min();
    for (const std::int16_t w : network.feature_weights) {
        if (w < min_w) min_w = w;
        if (w > max_w) max_w = w;
    }
    constexpr std::int64_t kMaxActivePlusDeltaTerms = 34;
    // Clamp the per-feature extrema at 0 before scaling: every partial sum from
    // the initial bias through kMaxActivePlusDeltaTerms additions must remain
    // representable in int16_t even when all weights share the same sign.
    const std::int64_t min_delta_w = std::min<std::int16_t>(0, min_w);
    const std::int64_t max_delta_w = std::max<std::int16_t>(0, max_w);
    return min_bias + kMaxActivePlusDeltaTerms * min_delta_w >=
            std::numeric_limits<std::int16_t>::min() &&
        max_bias + kMaxActivePlusDeltaTerms * max_delta_w <=
            std::numeric_limits<std::int16_t>::max();
}

[[nodiscard]] inline bool avx2_available() noexcept {
#if KOI_NNUE_COMPILED_AVX2
    return cpu_supports_avx2();
#else
    return false;
#endif
}

inline void accumulate_hidden_scalar(const NnueNetwork& network,
                              std::span<const std::int8_t> encoded,
                              std::vector<std::int32_t>& sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        std::int64_t activation = network.hidden_bias[hidden];
        for (std::size_t feature = 0; feature < encoded.size(); ++feature) {
            activation += static_cast<std::int64_t>(encoded[feature]) *
                network.feature_weights[feature * hidden_units + hidden];
        }
        activation = std::clamp<std::int64_t>(
            activation, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max());
        sums[hidden] = static_cast<std::int32_t>(activation);
    }
}

#if KOI_NNUE_COMPILED_AVX2
inline void accumulate_hidden_avx2(const NnueNetwork& network,
                            std::span<const std::int8_t> encoded,
                            std::vector<std::int32_t>& sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (hidden_units % 16U != 0) {
        accumulate_hidden_scalar(network, encoded, sums);
        return;
    }
    std::size_t active_features = 0;
    for (const std::int8_t feature : encoded) {
        if (feature == 0) {
            continue;
        }
        if (feature != 1) {
            accumulate_hidden_scalar(network, encoded, sums);
            return;
        }
        ++active_features;
    }
    const std::int64_t minimum_delta = static_cast<std::int64_t>(active_features) *
        std::numeric_limits<std::int16_t>::min();
    const std::int64_t maximum_delta = static_cast<std::int64_t>(active_features) *
        std::numeric_limits<std::int16_t>::max();
    for (const std::int32_t bias : network.hidden_bias) {
        const std::int64_t minimum = static_cast<std::int64_t>(bias) + minimum_delta;
        const std::int64_t maximum = static_cast<std::int64_t>(bias) + maximum_delta;
        if (minimum < std::numeric_limits<std::int32_t>::min() ||
            maximum > std::numeric_limits<std::int32_t>::max()) {
            accumulate_hidden_scalar(network, encoded, sums);
            return;
        }
    }
    for (std::size_t hidden = 0; hidden < hidden_units; hidden += 16U) {
        __m256i low = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden));
        __m256i high = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden + 8U));
        for (std::size_t feature = 0; feature < encoded.size(); ++feature) {
            if (encoded[feature] == 0) {
                continue;
            }
            const auto* feature_weights = network.feature_weights.data() +
                feature * hidden_units + hidden;
            const __m256i weights = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(feature_weights));
            const __m128i low_weights = _mm256_castsi256_si128(weights);
            const __m128i high_weights = _mm256_extracti128_si256(weights, 1);
            low = _mm256_add_epi32(low, _mm256_cvtepi16_epi32(low_weights));
            high = _mm256_add_epi32(high, _mm256_cvtepi16_epi32(high_weights));
        }
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(sums.data() + hidden), low);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(sums.data() + hidden + 8U), high);
    }
}
#endif

inline void accumulate_hidden_sparse_scalar(const NnueNetwork& network,
                                     std::span<const std::uint16_t> active,
                                     std::span<std::int32_t> sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        std::int64_t activation = network.hidden_bias[hidden];
        for (const std::uint16_t feature : active) {
            activation += network.feature_weights[
                static_cast<std::size_t>(feature) * hidden_units + hidden];
        }
        activation = std::clamp<std::int64_t>(
            activation, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max());
        sums[hidden] = static_cast<std::int32_t>(activation);
    }
}

inline void accumulate_hidden_sparse_16(const NnueNetwork& network,
                                 std::span<const std::uint16_t> active,
                                 std::span<std::int16_t> sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        sums[hidden] = static_cast<std::int16_t>(network.hidden_bias[hidden]);
    }
    for (const std::uint16_t feature : active) {
        const std::int16_t* const feature_weights = network.feature_weights.data() +
            static_cast<std::size_t>(feature) * hidden_units;
        std::size_t hidden = 0;
#if KOI_NNUE_COMPILED_NEON
        for (; hidden + 16U <= hidden_units; hidden += 16U) {
            vst1q_s16(sums.data() + hidden,
                      vaddq_s16(vld1q_s16(sums.data() + hidden),
                                vld1q_s16(feature_weights + hidden)));
            vst1q_s16(sums.data() + hidden + 8U,
                      vaddq_s16(vld1q_s16(sums.data() + hidden + 8U),
                                vld1q_s16(feature_weights + hidden + 8U)));
        }
        for (; hidden + 8U <= hidden_units; hidden += 8U) {
            vst1q_s16(sums.data() + hidden,
                      vaddq_s16(vld1q_s16(sums.data() + hidden),
                                vld1q_s16(feature_weights + hidden)));
        }
#elif KOI_NNUE_COMPILED_AVX2
        if (avx2_available()) {
            for (; hidden + 16U <= hidden_units; hidden += 16U) {
                const __m256i acc = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(sums.data() + hidden));
                const __m256i w = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(feature_weights + hidden));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(sums.data() + hidden),
                                    _mm256_add_epi16(acc, w));
            }
        }
#endif
        for (; hidden < hidden_units; ++hidden) {
            sums[hidden] = static_cast<std::int16_t>(sums[hidden] + feature_weights[hidden]);
        }
    }
}

#if KOI_NNUE_COMPILED_AVX2
inline void accumulate_hidden_sparse_avx2(const NnueNetwork& network,
                                   std::span<const std::uint16_t> active,
                                   std::span<std::int32_t> sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (hidden_units % 16U != 0) {
        accumulate_hidden_sparse_scalar(network, active, sums);
        return;
    }
    // The active features are binary inputs, so the first layer is a sum of
    // weight rows.  Bounding the accumulated delta keeps the int32 arithmetic
    // in range exactly like the dense path; the active list is tiny (tens of
    // entries), so this is safe for any realistic network here.
    const std::int64_t minimum_delta = static_cast<std::int64_t>(active.size()) *
        std::numeric_limits<std::int16_t>::min();
    const std::int64_t maximum_delta = static_cast<std::int64_t>(active.size()) *
        std::numeric_limits<std::int16_t>::max();
    for (const std::int32_t bias : network.hidden_bias) {
        if (static_cast<std::int64_t>(bias) + minimum_delta <
                std::numeric_limits<std::int32_t>::min() ||
            static_cast<std::int64_t>(bias) + maximum_delta >
                std::numeric_limits<std::int32_t>::max()) {
            accumulate_hidden_sparse_scalar(network, active, sums);
            return;
        }
    }
    for (std::size_t hidden = 0; hidden < hidden_units; hidden += 16U) {
        __m256i low = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden));
        __m256i high = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden + 8U));
        for (const std::uint16_t feature : active) {
            const auto* feature_weights = network.feature_weights.data() +
                static_cast<std::size_t>(feature) * hidden_units + hidden;
            const __m256i weights = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(feature_weights));
            const __m128i low_weights = _mm256_castsi256_si128(weights);
            const __m128i high_weights = _mm256_extracti128_si256(weights, 1);
            low = _mm256_add_epi32(low, _mm256_cvtepi16_epi32(low_weights));
            high = _mm256_add_epi32(high, _mm256_cvtepi16_epi32(high_weights));
        }
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(sums.data() + hidden), low);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(sums.data() + hidden + 8U), high);
    }
}
#endif

[[nodiscard]] inline int complete_inference(const NnueNetwork& network,
                                     std::span<const std::int32_t> hidden_sums,
                                     NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        accumulator.values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }

    std::int64_t output = network.output_bias;
#if KOI_NNUE_COMPILED_AVX2
    if (bottleneck_units == 8U && avx2_available()) {
        __m256i sums = _mm256_setzero_si256();
        for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
            const std::int32_t value = accumulator.values[hidden];
            const __m128i weights8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(
                network.bottleneck_weights.data() + hidden * bottleneck_units));
            const __m256i weights = _mm256_cvtepi8_epi32(weights8);
            sums = _mm256_add_epi32(
                sums, _mm256_mullo_epi32(_mm256_set1_epi32(value), weights));
        }
        std::int32_t lanes[8];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(lanes), sums);
        for (std::size_t bottleneck = 0; bottleneck < bottleneck_units; ++bottleneck) {
            std::int64_t activation = lanes[bottleneck];
            if (network.bottleneck_shift > 0) {
                activation >>= network.bottleneck_shift;
            }
            activation += network.bottleneck_bias[bottleneck];
            activation = std::clamp<std::int64_t>(activation, 0, kKoiNnueClippedReluMaximum);
            accumulator.bottleneck_values[bottleneck] = static_cast<std::int16_t>(activation);
            output += activation * network.output_weights[bottleneck];
        }
        if (network.output_shift > 0) {
            output >>= network.output_shift;
        }
        return clamp_score(output);
    }
#endif
    for (std::size_t bottleneck = 0; bottleneck < bottleneck_units; ++bottleneck) {
        std::int64_t activation = 0;
        for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
            activation += static_cast<std::int64_t>(accumulator.values[hidden]) *
                network.bottleneck_weights[hidden * bottleneck_units + bottleneck];
        }
        if (network.bottleneck_shift > 0) {
            activation >>= network.bottleneck_shift;
        }
        activation += network.bottleneck_bias[bottleneck];
        activation = std::clamp<std::int64_t>(activation, 0, kKoiNnueClippedReluMaximum);
        accumulator.bottleneck_values[bottleneck] = static_cast<std::int16_t>(activation);
        output += activation * network.output_weights[bottleneck];
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

[[nodiscard]] inline int infer_network(const NnueNetwork& network,
                                std::span<const std::int8_t> encoded,
                                NnueAccumulator& accumulator,
                                const bool use_avx2) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    if (encoded.size() != network.manifest.layer_sizes[0] || !arrays_match_manifest(network)) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }

    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(bottleneck_units, 0);
    std::vector<std::int32_t> hidden_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_avx2(network, encoded, hidden_sums);
    } else {
        accumulate_hidden_scalar(network, encoded, hidden_sums);
    }
#else
    (void)use_avx2;
    accumulate_hidden_scalar(network, encoded, hidden_sums);
#endif
    return complete_inference(network, hidden_sums, accumulator);
}

[[nodiscard]] inline int infer_network_sparse(const NnueNetwork& network,
                                       std::span<const std::uint16_t> active,
                                       NnueAccumulator& accumulator,
                                       const bool use_avx2) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    if (!arrays_match_manifest(network)) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }

    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(bottleneck_units, 0);
    std::vector<std::int32_t> hidden_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, active, hidden_sums);
    } else {
        accumulate_hidden_sparse_scalar(network, active, hidden_sums);
    }
#else
    (void)use_avx2;
    accumulate_hidden_sparse_scalar(network, active, hidden_sums);
#endif
    return complete_inference(network, hidden_sums, accumulator);
}

// v4 inference: the hidden layer is still a sum of binary feature rows, then
// the clipped activations feed pair products p[j] = a[j] * a[j + hidden/2]
// and one linear head selected by the piece-count bucket.
[[nodiscard]] inline std::size_t output_bucket_for_pieces(const std::size_t pieces) noexcept {
    const std::size_t missing = 32U - std::min<std::size_t>(32U, pieces);
    return std::min<std::size_t>(7U, missing / 4U);
}

[[nodiscard]] inline std::size_t piece_count_of(const EvaluationFeatures& features) noexcept {
    std::size_t pieces = 0;
    for (const Piece& piece : features.position.board) {
        if (!piece.empty()) {
            ++pieces;
        }
    }
    return pieces;
}

[[nodiscard]] inline std::size_t piece_count_bucket(const EvaluationFeatures& features) noexcept {
    return output_bucket_for_pieces(piece_count_of(features));
}

[[nodiscard]] inline constexpr PieceType promotion_piece_type(const Promotion promotion) noexcept {
    switch (promotion) {
        case Promotion::knight: return PieceType::knight;
        case Promotion::bishop: return PieceType::bishop;
        case Promotion::rook: return PieceType::rook;
        case Promotion::queen: return PieceType::queen;
        default: return PieceType::pawn;
    }
}

#if KOI_NNUE_COMPILED_AVX2
template <typename SumT>
void fill_v4_activations_avx2_t(std::span<const SumT> hidden_sums,
                                std::span<std::int16_t> values) noexcept {
    std::size_t hidden = 0;
    const __m256i zero = _mm256_setzero_si256();
    if constexpr (std::is_same_v<SumT, std::int16_t>) {
        const __m256i maximum =
            _mm256_set1_epi16(static_cast<std::int16_t>(kKoiNnueClippedReluMaximum));
        for (; hidden + 16U <= hidden_sums.size(); hidden += 16U) {
            __m256i v = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(hidden_sums.data() + hidden));
            v = _mm256_min_epi16(_mm256_max_epi16(v, zero), maximum);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(values.data() + hidden), v);
        }
    } else {
        const __m256i maximum = _mm256_set1_epi32(kKoiNnueClippedReluMaximum);
        for (; hidden + 16U <= hidden_sums.size(); hidden += 16U) {
            __m256i low = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(hidden_sums.data() + hidden));
            __m256i high = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(hidden_sums.data() + hidden + 8U));
            low = _mm256_min_epi32(_mm256_max_epi32(low, zero), maximum);
            high = _mm256_min_epi32(_mm256_max_epi32(high, zero), maximum);
            __m256i packed = _mm256_packs_epi32(low, high);
            packed = _mm256_permute4x64_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(values.data() + hidden), packed);
        }
    }
    for (; hidden < hidden_sums.size(); ++hidden) {
        values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }
}

inline void fill_v4_activations_avx2(std::span<const std::int32_t> hidden_sums,
                              std::span<std::int16_t> values) noexcept {
    fill_v4_activations_avx2_t<std::int32_t>(hidden_sums, values);
}
#endif

#if KOI_NNUE_COMPILED_NEON
inline void fill_v4_activations_neon(std::span<const std::int32_t> hidden_sums,
                              std::span<std::int16_t> values) noexcept {
    std::size_t hidden = 0;
    const int32x4_t zero = vdupq_n_s32(0);
    const int32x4_t maximum = vdupq_n_s32(kKoiNnueClippedReluMaximum);
    for (; hidden + 8U <= hidden_sums.size(); hidden += 8U) {
        int32x4_t low = vld1q_s32(hidden_sums.data() + hidden);
        int32x4_t high = vld1q_s32(hidden_sums.data() + hidden + 4U);
        low = vminq_s32(vmaxq_s32(low, zero), maximum);
        high = vminq_s32(vmaxq_s32(high, zero), maximum);
        const int16x8_t packed = vcombine_s16(vqmovn_s32(low), vqmovn_s32(high));
        vst1q_s16(values.data() + hidden, packed);
    }
    for (; hidden < hidden_sums.size(); ++hidden) {
        values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }
}

inline void fill_v4_activations_neon_16(std::span<const std::int16_t> hidden_sums,
                                 std::span<std::int16_t> values) noexcept {
    std::size_t hidden = 0;
    const int16x8_t zero = vdupq_n_s16(0);
    const int16x8_t maximum =
        vdupq_n_s16(static_cast<std::int16_t>(kKoiNnueClippedReluMaximum));
    for (; hidden + 8U <= hidden_sums.size(); hidden += 8U) {
        vst1q_s16(values.data() + hidden,
                  vminq_s16(vmaxq_s16(vld1q_s16(hidden_sums.data() + hidden), zero),
                            maximum));
    }
    for (; hidden < hidden_sums.size(); ++hidden) {
        values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }
}
#endif

template <typename SumT>
void fill_v4_activations_span_t(std::span<const SumT> hidden_sums,
                                std::span<std::int16_t> values) noexcept {
#if KOI_NNUE_COMPILED_NEON
    if constexpr (std::is_same_v<SumT, std::int16_t>) {
        fill_v4_activations_neon_16(hidden_sums, values);
    } else {
        fill_v4_activations_neon(hidden_sums, values);
    }
    return;
#endif
#if KOI_NNUE_COMPILED_AVX2
    if (avx2_available()) {
        fill_v4_activations_avx2_t<SumT>(hidden_sums, values);
        return;
    }
#endif
    for (std::size_t hidden = 0; hidden < hidden_sums.size(); ++hidden) {
        values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }
}

template <typename SumT>
void fill_v4_activations_t(std::span<const SumT> hidden_sums,
                           NnueAccumulator& accumulator) noexcept {
    fill_v4_activations_span_t<SumT>(hidden_sums, accumulator.values);
}

template <typename SumT>
[[nodiscard]] int finish_inference_v4_scalar_t(const NnueNetwork& network,
                                               std::span<const SumT> hidden_sums,
                                               const std::size_t bucket,
                                               NnueAccumulator& accumulator) noexcept {
    const std::size_t pair_count = network.manifest.layer_sizes[1] / 2U;
    fill_v4_activations_t<SumT>(hidden_sums, accumulator);
    const auto* weights = network.bottleneck_weights.data() + bucket * pair_count;
    std::int64_t output = network.bottleneck_bias[bucket];
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
        const std::int16_t pair_product = static_cast<std::int16_t>(
            accumulator.values[pair] * accumulator.values[pair + pair_count]);
        accumulator.bottleneck_values[pair] = pair_product;
        output += static_cast<std::int64_t>(weights[pair]) * pair_product;
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

[[nodiscard]] inline int finish_inference_v4_scalar(const NnueNetwork& network,
                                             std::span<const std::int32_t> hidden_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    return finish_inference_v4_scalar_t<std::int32_t>(
        network, hidden_sums, bucket, accumulator);
}

template <typename SumT>
[[nodiscard]] int finish_inference_v4_dual_scalar_t(const NnueNetwork& network,
                                                    std::span<const SumT> own_sums,
                                                    std::span<const SumT> opp_sums,
                                                    const std::size_t bucket,
                                                    NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden_units / 2U;
    std::span<std::int16_t> own_act(accumulator.values.data(), hidden_units);
    std::span<std::int16_t> opp_act(accumulator.values.data() + hidden_units, hidden_units);
    fill_v4_activations_span_t<SumT>(own_sums, own_act);
    fill_v4_activations_span_t<SumT>(opp_sums, opp_act);
    const auto* weights = network.bottleneck_weights.data() + bucket * hidden_units;
    std::int64_t output = network.bottleneck_bias[bucket];
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
        const std::int16_t own_product = static_cast<std::int16_t>(
            own_act[pair] * own_act[pair + pair_count]);
        const std::int16_t opp_product = static_cast<std::int16_t>(
            opp_act[pair] * opp_act[pair + pair_count]);
        accumulator.bottleneck_values[pair] = own_product;
        accumulator.bottleneck_values[pair + pair_count] = opp_product;
        output += static_cast<std::int64_t>(weights[pair]) * own_product;
        output += static_cast<std::int64_t>(weights[pair + pair_count]) * opp_product;
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

[[nodiscard]] inline int finish_inference_v4_dual_scalar(const NnueNetwork& network,
                                                  std::span<const std::int32_t> own_sums,
                                                  std::span<const std::int32_t> opp_sums,
                                                  const std::size_t bucket,
                                                  NnueAccumulator& accumulator) noexcept {
    return finish_inference_v4_dual_scalar_t<std::int32_t>(
        network, own_sums, opp_sums, bucket, accumulator);
}

#if KOI_NNUE_COMPILED_NEON
#if defined(__clang__) || defined(__GNUC__)
#define KOI_NNUE_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define KOI_NNUE_ALWAYS_INLINE inline
#endif

template <typename SumT, bool WriteAccumulator = true>
KOI_NNUE_ALWAYS_INLINE void accumulate_v4_perspective_fused_neon_t(
    std::span<const SumT> sums,
    const std::int8_t* weights,
    const std::size_t pair_count,
    std::span<std::int16_t> activations,
    std::span<std::int16_t> pairs,
    int32x4_t& acc_low,
    int32x4_t& acc_high,
    std::int64_t& acc_tail) noexcept {
    std::size_t pair = 0;
    if constexpr (std::is_same_v<SumT, std::int16_t>) {
        const int16x8_t zero = vdupq_n_s16(0);
        const int16x8_t maximum =
            vdupq_n_s16(static_cast<std::int16_t>(kKoiNnueClippedReluMaximum));
        if constexpr (!WriteAccumulator) {
            int32x4_t acc2 = vdupq_n_s32(0);
            int32x4_t acc3 = vdupq_n_s32(0);
            for (; pair + 16U <= pair_count; pair += 16U) {
                const int16x8_t act0_lo =
                    vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + pair), zero), maximum);
                const int16x8_t act0_hi =
                    vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + pair + 8U), zero), maximum);
                const int16x8_t act1_lo =
                    vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + pair + pair_count), zero), maximum);
                const int16x8_t act1_hi =
                    vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + pair + pair_count + 8U), zero), maximum);
                const int16x8_t prod_lo = vmulq_s16(act0_lo, act1_lo);
                const int16x8_t prod_hi = vmulq_s16(act0_hi, act1_hi);
                const int8x16_t weight_bytes = vld1q_s8(weights + pair);
                const int16x8_t w_lo = vmovl_s8(vget_low_s8(weight_bytes));
#if defined(__aarch64__)
                const int16x8_t w_hi = vmovl_high_s8(weight_bytes);
                acc_low = vmlal_s16(acc_low, vget_low_s16(prod_lo), vget_low_s16(w_lo));
                acc_high = vmlal_high_s16(acc_high, prod_lo, w_lo);
                acc2 = vmlal_s16(acc2, vget_low_s16(prod_hi), vget_low_s16(w_hi));
                acc3 = vmlal_high_s16(acc3, prod_hi, w_hi);
#else
                const int16x8_t w_hi = vmovl_s8(vget_high_s8(weight_bytes));
                acc_low = vmlal_s16(acc_low, vget_low_s16(prod_lo), vget_low_s16(w_lo));
                acc_high = vmlal_s16(acc_high, vget_high_s16(prod_lo), vget_high_s16(w_lo));
                acc2 = vmlal_s16(acc2, vget_low_s16(prod_hi), vget_low_s16(w_hi));
                acc3 = vmlal_s16(acc3, vget_high_s16(prod_hi), vget_high_s16(w_hi));
#endif
            }
            acc_low = vaddq_s32(acc_low, acc2);
            acc_high = vaddq_s32(acc_high, acc3);
        }
        for (; pair + 8U <= pair_count; pair += 8U) {
            const int16x8_t act0 =
                vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + pair), zero), maximum);
            const int16x8_t act1 =
                vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + pair + pair_count), zero), maximum);
            if constexpr (WriteAccumulator) {
                vst1q_s16(activations.data() + pair, act0);
                vst1q_s16(activations.data() + pair + pair_count, act1);
            }
            const int16x8_t prod = vmulq_s16(act0, act1);
            if constexpr (WriteAccumulator) {
                vst1q_s16(pairs.data() + pair, prod);
            }
            const int16x8_t weight_values = vmovl_s8(vld1_s8(weights + pair));
            acc_low = vmlal_s16(acc_low, vget_low_s16(prod), vget_low_s16(weight_values));
            acc_high = vmlal_s16(acc_high, vget_high_s16(prod), vget_high_s16(weight_values));
        }
    } else {
        const int32x4_t zero = vdupq_n_s32(0);
        const int32x4_t maximum = vdupq_n_s32(kKoiNnueClippedReluMaximum);
        for (; pair + 8U <= pair_count; pair += 8U) {
            int32x4_t low0 = vld1q_s32(sums.data() + pair);
            int32x4_t high0 = vld1q_s32(sums.data() + pair + 4U);
            int32x4_t low1 = vld1q_s32(sums.data() + pair + pair_count);
            int32x4_t high1 = vld1q_s32(sums.data() + pair + pair_count + 4U);
            low0 = vminq_s32(vmaxq_s32(low0, zero), maximum);
            high0 = vminq_s32(vmaxq_s32(high0, zero), maximum);
            low1 = vminq_s32(vmaxq_s32(low1, zero), maximum);
            high1 = vminq_s32(vmaxq_s32(high1, zero), maximum);
            const int16x8_t act0 = vcombine_s16(vqmovn_s32(low0), vqmovn_s32(high0));
            const int16x8_t act1 = vcombine_s16(vqmovn_s32(low1), vqmovn_s32(high1));
            if constexpr (WriteAccumulator) {
                vst1q_s16(activations.data() + pair, act0);
                vst1q_s16(activations.data() + pair + pair_count, act1);
            }
            const int16x8_t prod = vmulq_s16(act0, act1);
            if constexpr (WriteAccumulator) {
                vst1q_s16(pairs.data() + pair, prod);
            }
            const int16x8_t weight_values = vmovl_s8(vld1_s8(weights + pair));
            acc_low = vmlal_s16(acc_low, vget_low_s16(prod), vget_low_s16(weight_values));
            acc_high = vmlal_s16(acc_high, vget_high_s16(prod), vget_high_s16(weight_values));
        }
    }
    for (; pair < pair_count; ++pair) {
        const std::int16_t a0 = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            sums[pair], 0, kKoiNnueClippedReluMaximum));
        const std::int16_t a1 = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            sums[pair + pair_count], 0, kKoiNnueClippedReluMaximum));
        if constexpr (WriteAccumulator) {
            activations[pair] = a0;
            activations[pair + pair_count] = a1;
        }
        const std::int16_t product = static_cast<std::int16_t>(a0 * a1);
        if constexpr (WriteAccumulator) {
            pairs[pair] = product;
        }
        acc_tail += static_cast<std::int64_t>(weights[pair]) * product;
    }
}

template <typename SumT, bool WriteAccumulator = true>
[[nodiscard]] int finish_inference_v4_neon_t(const NnueNetwork& network,
                                             std::span<const SumT> hidden_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    const std::size_t pair_count = network.manifest.layer_sizes[1] / 2U;
    constexpr std::int64_t kMaximumPairProduct = 127 * 127;
    if ((hidden_sums.size() & 1U) != 0 ||
        static_cast<std::int64_t>(pair_count) * 127 * kMaximumPairProduct >
            std::numeric_limits<std::int32_t>::max()) {
        return finish_inference_v4_scalar_t<SumT>(network, hidden_sums, bucket, accumulator);
    }
    const auto* weights = network.bottleneck_weights.data() + bucket * pair_count;
    int32x4_t acc_low = vdupq_n_s32(0);
    int32x4_t acc_high = vdupq_n_s32(0);
    std::int64_t acc_tail = 0;
    accumulate_v4_perspective_fused_neon_t<SumT, WriteAccumulator>(
        hidden_sums, weights, pair_count, accumulator.values,
        accumulator.bottleneck_values, acc_low, acc_high, acc_tail);
    std::int64_t output = network.bottleneck_bias[bucket] +
        static_cast<std::int64_t>(vaddvq_s32(vaddq_s32(acc_low, acc_high))) +
        acc_tail;
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

[[nodiscard]] inline int finish_inference_v4_neon(const NnueNetwork& network,
                                           std::span<const std::int32_t> hidden_sums,
                                           const std::size_t bucket,
                                           NnueAccumulator& accumulator) noexcept {
    return finish_inference_v4_neon_t<std::int32_t>(
        network, hidden_sums, bucket, accumulator);
}

template <typename SumT, bool WriteAccumulator = true>
[[nodiscard]] int finish_inference_v4_dual_neon_t(const NnueNetwork& network,
                                                  std::span<const SumT> own_sums,
                                                  std::span<const SumT> opp_sums,
                                                  const std::size_t bucket,
                                                  NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden_units / 2U;
    const std::size_t total_pairs = hidden_units;
    constexpr std::int64_t kMaximumPairProduct = 127 * 127;
    if ((hidden_units & 1U) != 0 ||
        static_cast<std::int64_t>(total_pairs) * 127 * kMaximumPairProduct >
            std::numeric_limits<std::int32_t>::max()) {
        return finish_inference_v4_dual_scalar_t<SumT>(
            network, own_sums, opp_sums, bucket, accumulator);
    }
    const auto* weights = network.bottleneck_weights.data() + bucket * total_pairs;
    std::span<std::int16_t> own_act;
    std::span<std::int16_t> opp_act;
    std::span<std::int16_t> own_pairs;
    std::span<std::int16_t> opp_pairs;
    if constexpr (WriteAccumulator) {
        own_act = std::span<std::int16_t>(accumulator.values.data(), hidden_units);
        opp_act = std::span<std::int16_t>(accumulator.values.data() + hidden_units, hidden_units);
        own_pairs = std::span<std::int16_t>(accumulator.bottleneck_values.data(), pair_count);
        opp_pairs = std::span<std::int16_t>(
            accumulator.bottleneck_values.data() + pair_count, pair_count);
    }
    int32x4_t acc_low = vdupq_n_s32(0);
    int32x4_t acc_high = vdupq_n_s32(0);
    std::int64_t acc_tail = 0;
    accumulate_v4_perspective_fused_neon_t<SumT, WriteAccumulator>(
        own_sums, weights, pair_count, own_act, own_pairs, acc_low, acc_high, acc_tail);
    accumulate_v4_perspective_fused_neon_t<SumT, WriteAccumulator>(
        opp_sums, weights + pair_count, pair_count, opp_act, opp_pairs, acc_low, acc_high, acc_tail);
    std::int64_t output = network.bottleneck_bias[bucket] +
        static_cast<std::int64_t>(vaddvq_s32(vaddq_s32(acc_low, acc_high))) +
        acc_tail;
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

[[nodiscard]] inline int finish_inference_v4_dual_neon(const NnueNetwork& network,
                                                std::span<const std::int32_t> own_sums,
                                                std::span<const std::int32_t> opp_sums,
                                                const std::size_t bucket,
                                                NnueAccumulator& accumulator) noexcept {
    return finish_inference_v4_dual_neon_t<std::int32_t>(
        network, own_sums, opp_sums, bucket, accumulator);
}
#undef KOI_NNUE_ALWAYS_INLINE
#endif

#if KOI_NNUE_COMPILED_AVX2
inline void compute_pair_products_avx2(std::span<const std::int16_t> activations,
                                std::span<std::int16_t> pairs) noexcept {
    const std::size_t pair_count = pairs.size();
    std::size_t pair = 0;
    for (; pair + 16U <= pair_count; pair += 16U) {
        const __m256i low = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(activations.data() + pair));
        const __m256i high = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(activations.data() + pair + pair_count));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(pairs.data() + pair),
                            _mm256_mullo_epi16(low, high));
    }
    for (; pair < pair_count; ++pair) {
        pairs[pair] = static_cast<std::int16_t>(
            activations[pair] * activations[pair + pair_count]);
    }
}

[[nodiscard]] inline std::int64_t bucket_dot_avx2(const NnueNetwork& network,
                                           const std::size_t bucket,
                                           std::span<const std::int16_t> pairs) noexcept {
    const std::size_t pair_count = pairs.size();
    const auto* weights = network.bottleneck_weights.data() + bucket * pair_count;
    constexpr std::int64_t kMaximumPairProduct = 127 * 127;
    if (static_cast<std::int64_t>(pair_count) * 127 * kMaximumPairProduct >
        std::numeric_limits<std::int32_t>::max()) {
        std::int64_t output = 0;
        for (std::size_t pair = 0; pair < pair_count; ++pair) {
            output += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
        }
        return output;
    }
    __m256i accumulator = _mm256_setzero_si256();
    std::size_t pair = 0;
    for (; pair + 16U <= pair_count; pair += 16U) {
        const __m256i pair_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(pairs.data() + pair));
        const __m128i weight_bytes = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(weights + pair));
        const __m256i weight_values = _mm256_cvtepi8_epi16(weight_bytes);
        accumulator = _mm256_add_epi32(
            accumulator, _mm256_madd_epi16(pair_values, weight_values));
    }
    alignas(32) std::array<std::int32_t, 8> lanes{};
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes.data()), accumulator);
    std::int64_t output = 0;
    for (const std::int32_t lane : lanes) {
        output += lane;
    }
    for (; pair < pair_count; ++pair) {
        output += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
    }
    return output;
}

template <typename SumT>
[[nodiscard]] int finish_inference_v4_avx2_t(const NnueNetwork& network,
                                             std::span<const SumT> hidden_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    fill_v4_activations_t<SumT>(hidden_sums, accumulator);
    compute_pair_products_avx2(accumulator.values, accumulator.bottleneck_values);
    std::int64_t output = network.bottleneck_bias[bucket] +
        bucket_dot_avx2(network, bucket, accumulator.bottleneck_values);
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

[[nodiscard]] inline int finish_inference_v4_avx2(const NnueNetwork& network,
                                           std::span<const std::int32_t> hidden_sums,
                                           const std::size_t bucket,
                                           NnueAccumulator& accumulator) noexcept {
    return finish_inference_v4_avx2_t<std::int32_t>(
        network, hidden_sums, bucket, accumulator);
}

template <typename SumT>
[[nodiscard]] int finish_inference_v4_dual_avx2_t(const NnueNetwork& network,
                                                  std::span<const SumT> own_sums,
                                                  std::span<const SumT> opp_sums,
                                                  const std::size_t bucket,
                                                  NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden_units / 2U;
    std::span<std::int16_t> own_act(accumulator.values.data(), hidden_units);
    std::span<std::int16_t> opp_act(accumulator.values.data() + hidden_units, hidden_units);
    fill_v4_activations_avx2_t<SumT>(own_sums, own_act);
    fill_v4_activations_avx2_t<SumT>(opp_sums, opp_act);
    compute_pair_products_avx2(
        own_act, std::span<std::int16_t>(accumulator.bottleneck_values.data(), pair_count));
    compute_pair_products_avx2(
        opp_act,
        std::span<std::int16_t>(accumulator.bottleneck_values.data() + pair_count, pair_count));
    std::int64_t output = network.bottleneck_bias[bucket] +
        bucket_dot_avx2(network, bucket, accumulator.bottleneck_values);
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

[[nodiscard]] inline int finish_inference_v4_dual_avx2(const NnueNetwork& network,
                                                std::span<const std::int32_t> own_sums,
                                                std::span<const std::int32_t> opp_sums,
                                                const std::size_t bucket,
                                                NnueAccumulator& accumulator) noexcept {
    return finish_inference_v4_dual_avx2_t<std::int32_t>(
        network, own_sums, opp_sums, bucket, accumulator);
}
#endif

[[nodiscard]] inline int infer_network_sparse_v4(const NnueNetwork& network,
                                          std::span<const std::uint16_t> active,
                                          const std::size_t bucket,
                                          NnueAccumulator& accumulator,
                                          const bool use_avx2,
                                          const bool use_simd = true) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (!arrays_match_manifest(network) || bucket >= network.manifest.layer_sizes[2]) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }
    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(hidden_units / 2U, 0);
    std::vector<std::int32_t> hidden_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, active, hidden_sums);
        return finish_inference_v4_avx2(network, hidden_sums, bucket, accumulator);
    }
#else
    (void)use_avx2;
#endif
    accumulate_hidden_sparse_scalar(network, active, hidden_sums);
#if KOI_NNUE_COMPILED_NEON
    if (use_simd) {
        return finish_inference_v4_neon(network, hidden_sums, bucket, accumulator);
    }
#else
    (void)use_simd;
#endif
    return finish_inference_v4_scalar(network, hidden_sums, bucket, accumulator);
}

[[nodiscard]] inline int infer_network_sparse_v4_dual(const NnueNetwork& network,
                                               std::span<const std::uint16_t> own_active,
                                               std::span<const std::uint16_t> opp_active,
                                               const std::size_t bucket,
                                               NnueAccumulator& accumulator,
                                               const bool use_avx2,
                                               const bool use_simd = true) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (!arrays_match_manifest(network) || bucket >= network.manifest.layer_sizes[2]) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }
    accumulator.values.assign(hidden_units * 2U, 0);
    accumulator.bottleneck_values.assign(hidden_units, 0);
    std::vector<std::int32_t> own_sums(hidden_units, 0);
    std::vector<std::int32_t> opp_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, own_active, own_sums);
        accumulate_hidden_sparse_avx2(network, opp_active, opp_sums);
        return finish_inference_v4_dual_avx2(network, own_sums, opp_sums, bucket, accumulator);
    }
#else
    (void)use_avx2;
#endif
    accumulate_hidden_sparse_scalar(network, own_active, own_sums);
    accumulate_hidden_sparse_scalar(network, opp_active, opp_sums);
#if KOI_NNUE_COMPILED_NEON
    if (use_simd) {
        return finish_inference_v4_dual_neon(network, own_sums, opp_sums, bucket, accumulator);
    }
#else
    (void)use_simd;
#endif
    return finish_inference_v4_dual_scalar(network, own_sums, opp_sums, bucket, accumulator);
}

// v9 inference (`halfka-hm-v2`): 10 horizontally-mirrored king buckets,
// dual-perspective SCReLU clipped to [0, qa], int8 head weights (qb),
// scaled by eval_scale / (qa * qa * qb) via truncating integer division.
[[nodiscard]] inline int scale_v9_output(const NnueNetwork& network,
                                  const std::size_t bucket,
                                  const std::int64_t dot_sum) noexcept {
    const std::int64_t total =
        static_cast<std::int64_t>(network.bottleneck_bias[bucket]) + dot_sum;
    const std::int64_t denom =
        static_cast<std::int64_t>(network.qa) * network.qa * network.qb;
    return clamp_score((total * static_cast<std::int64_t>(network.eval_scale)) / denom);
}

template <typename SumT, bool WriteAccumulator = true>
[[nodiscard]] int finish_inference_v9_scalar_t(const NnueNetwork& network,
                                               std::span<const SumT> own_sums,
                                               std::span<const SumT> opp_sums,
                                               const std::size_t bucket,
                                               NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::int32_t qa = static_cast<std::int32_t>(network.qa);
    const auto* const own_weights =
        network.bottleneck_weights.data() + bucket * (2U * hidden_units);
    const auto* const opp_weights = own_weights + hidden_units;
    std::int64_t dot_sum = 0;
    for (std::size_t i = 0; i < hidden_units; ++i) {
        const std::int32_t a = std::clamp<std::int32_t>(own_sums[i], 0, qa);
        if constexpr (WriteAccumulator) {
            accumulator.values[i] = static_cast<std::int16_t>(a);
        }
        dot_sum += static_cast<std::int64_t>(a) * a * own_weights[i];
    }
    for (std::size_t i = 0; i < hidden_units; ++i) {
        const std::int32_t a = std::clamp<std::int32_t>(opp_sums[i], 0, qa);
        if constexpr (WriteAccumulator) {
            accumulator.values[i + hidden_units] = static_cast<std::int16_t>(a);
        }
        dot_sum += static_cast<std::int64_t>(a) * a * opp_weights[i];
    }
    return scale_v9_output(network, bucket, dot_sum);
}

#if KOI_NNUE_COMPILED_NEON
template <typename SumT, bool WriteAccumulator = true>
[[nodiscard]] int finish_inference_v9_neon_t(const NnueNetwork& network,
                                             std::span<const SumT> own_sums,
                                             std::span<const SumT> opp_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::int32_t qa_i32 = static_cast<std::int32_t>(network.qa);
    const std::int64_t terms_per_lane =
        static_cast<std::int64_t>((2U * hidden_units + 15U) / 16U);
    if (qa_i32 > 255 ||
        terms_per_lane * qa_i32 * qa_i32 * 128 > std::numeric_limits<std::int32_t>::max()) {
        return finish_inference_v9_scalar_t<SumT, WriteAccumulator>(
            network, own_sums, opp_sums, bucket, accumulator);
    }
    const std::int16_t qa_16 = static_cast<std::int16_t>(network.qa);
    const int16x8_t zero_16 = vdupq_n_s16(0);
    const int16x8_t max_16 = vdupq_n_s16(qa_16);
    const int32x4_t zero_32 = vdupq_n_s32(0);
    const int32x4_t max_32 = vdupq_n_s32(qa_i32);
    int32x4_t acc0 = vdupq_n_s32(0);
    int32x4_t acc1 = vdupq_n_s32(0);
    int32x4_t acc2 = vdupq_n_s32(0);
    int32x4_t acc3 = vdupq_n_s32(0);
    std::int64_t acc_tail = 0;

    const auto accum_persp = [&](std::span<const SumT> sums,
                                 const std::int8_t* weights,
                                 std::span<std::int16_t> out_act) noexcept {
        std::size_t hidden = 0;
        for (; hidden + 16U <= hidden_units; hidden += 16U) {
            int16x8_t a_lo;
            int16x8_t a_hi;
            if constexpr (std::is_same_v<SumT, std::int16_t>) {
                a_lo = vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + hidden), zero_16), max_16);
                a_hi = vminq_s16(vmaxq_s16(vld1q_s16(sums.data() + hidden + 8U), zero_16), max_16);
            } else {
                const int32x4_t s0 = vminq_s32(vmaxq_s32(vld1q_s32(sums.data() + hidden), zero_32), max_32);
                const int32x4_t s1 = vminq_s32(vmaxq_s32(vld1q_s32(sums.data() + hidden + 4U), zero_32), max_32);
                const int32x4_t s2 = vminq_s32(vmaxq_s32(vld1q_s32(sums.data() + hidden + 8U), zero_32), max_32);
                const int32x4_t s3 = vminq_s32(vmaxq_s32(vld1q_s32(sums.data() + hidden + 12U), zero_32), max_32);
                a_lo = vcombine_s16(vqmovn_s32(s0), vqmovn_s32(s1));
                a_hi = vcombine_s16(vqmovn_s32(s2), vqmovn_s32(s3));
            }
            if constexpr (WriteAccumulator) {
                vst1q_s16(out_act.data() + hidden, a_lo);
                vst1q_s16(out_act.data() + hidden + 8U, a_hi);
            }
            const int8x16_t wb = vld1q_s8(weights + hidden);
            const int16x8_t w_lo = vmovl_s8(vget_low_s8(wb));
#if defined(__aarch64__)
            const int16x8_t w_hi = vmovl_high_s8(wb);
#else
            const int16x8_t w_hi = vmovl_s8(vget_high_s8(wb));
#endif
            const int16x8_t aw_lo = vmulq_s16(a_lo, w_lo);
            const int16x8_t aw_hi = vmulq_s16(a_hi, w_hi);
            acc0 = vmlal_s16(acc0, vget_low_s16(aw_lo), vget_low_s16(a_lo));
            acc2 = vmlal_s16(acc2, vget_low_s16(aw_hi), vget_low_s16(a_hi));
#if defined(__aarch64__)
            acc1 = vmlal_high_s16(acc1, aw_lo, a_lo);
            acc3 = vmlal_high_s16(acc3, aw_hi, a_hi);
#else
            acc1 = vmlal_s16(acc1, vget_high_s16(aw_lo), vget_high_s16(a_lo));
            acc3 = vmlal_s16(acc3, vget_high_s16(aw_hi), vget_high_s16(a_hi));
#endif
        }
        for (; hidden < hidden_units; ++hidden) {
            const std::int32_t a = std::clamp<std::int32_t>(sums[hidden], 0, qa_i32);
            if constexpr (WriteAccumulator) {
                out_act[hidden] = static_cast<std::int16_t>(a);
            }
            acc_tail += static_cast<std::int64_t>(a) * a * weights[hidden];
        }
    };

    const std::int8_t* const own_weights =
        network.bottleneck_weights.data() + bucket * (2U * hidden_units);
    const std::int8_t* const opp_weights = own_weights + hidden_units;
    std::span<std::int16_t> own_act;
    std::span<std::int16_t> opp_act;
    if constexpr (WriteAccumulator) {
        own_act = std::span<std::int16_t>(accumulator.values.data(), hidden_units);
        opp_act = std::span<std::int16_t>(accumulator.values.data() + hidden_units, hidden_units);
    }
    accum_persp(own_sums, own_weights, own_act);
    accum_persp(opp_sums, opp_weights, opp_act);

#if defined(__aarch64__)
    const std::int64_t dot_sum =
        static_cast<std::int64_t>(vaddlvq_s32(acc0)) +
        static_cast<std::int64_t>(vaddlvq_s32(acc1)) +
        static_cast<std::int64_t>(vaddlvq_s32(acc2)) +
        static_cast<std::int64_t>(vaddlvq_s32(acc3)) + acc_tail;
#else
    const auto sum_lanes_64 = [](const int32x4_t v) noexcept -> std::int64_t {
        std::array<std::int32_t, 4> tmp{};
        vst1q_s32(tmp.data(), v);
        return static_cast<std::int64_t>(tmp[0]) + tmp[1] + tmp[2] + tmp[3];
    };
    const std::int64_t dot_sum =
        sum_lanes_64(acc0) + sum_lanes_64(acc1) +
        sum_lanes_64(acc2) + sum_lanes_64(acc3) + acc_tail;
#endif
    return scale_v9_output(network, bucket, dot_sum);
}
#endif

#if KOI_NNUE_COMPILED_AVX2
template <typename SumT, bool WriteAccumulator = true>
[[nodiscard]] int finish_inference_v9_avx2_t(const NnueNetwork& network,
                                             std::span<const SumT> own_sums,
                                             std::span<const SumT> opp_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::int32_t qa_i32 = static_cast<std::int32_t>(network.qa);
    const std::int64_t terms_per_lane =
        static_cast<std::int64_t>((2U * hidden_units + 15U) / 16U) * 2;
    if (qa_i32 > 255 ||
        terms_per_lane * qa_i32 * qa_i32 * 128 > std::numeric_limits<std::int32_t>::max()) {
        return finish_inference_v9_scalar_t<SumT, WriteAccumulator>(
            network, own_sums, opp_sums, bucket, accumulator);
    }
    const std::int16_t qa_16 = static_cast<std::int16_t>(network.qa);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i max_16 = _mm256_set1_epi16(qa_16);
    const __m256i max_32 = _mm256_set1_epi32(qa_i32);
    __m256i acc = _mm256_setzero_si256();
    std::int64_t acc_tail = 0;

    const auto accum_persp = [&](std::span<const SumT> sums,
                                 const std::int8_t* weights,
                                 std::span<std::int16_t> out_act) noexcept {
        std::size_t hidden = 0;
        for (; hidden + 16U <= hidden_units; hidden += 16U) {
            __m256i a;
            if constexpr (std::is_same_v<SumT, std::int16_t>) {
                const __m256i raw = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(sums.data() + hidden));
                a = _mm256_min_epi16(_mm256_max_epi16(raw, zero), max_16);
            } else {
                __m256i lo = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(sums.data() + hidden));
                __m256i hi = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(sums.data() + hidden + 8U));
                lo = _mm256_min_epi32(_mm256_max_epi32(lo, zero), max_32);
                hi = _mm256_min_epi32(_mm256_max_epi32(hi, zero), max_32);
                a = _mm256_packs_epi32(lo, hi);
                a = _mm256_permute4x64_epi64(a, _MM_SHUFFLE(3, 1, 2, 0));
            }
            if constexpr (WriteAccumulator) {
                _mm256_storeu_si256(
                    reinterpret_cast<__m256i*>(out_act.data() + hidden), a);
            }
            const __m256i w = _mm256_cvtepi8_epi16(_mm_loadu_si128(
                reinterpret_cast<const __m128i*>(weights + hidden)));
            const __m256i aw = _mm256_mullo_epi16(a, w);
            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(aw, a));
        }
        for (; hidden < hidden_units; ++hidden) {
            const std::int32_t a = std::clamp<std::int32_t>(sums[hidden], 0, qa_i32);
            if constexpr (WriteAccumulator) {
                out_act[hidden] = static_cast<std::int16_t>(a);
            }
            acc_tail += static_cast<std::int64_t>(a) * a * weights[hidden];
        }
    };

    const std::int8_t* const own_weights =
        network.bottleneck_weights.data() + bucket * (2U * hidden_units);
    const std::int8_t* const opp_weights = own_weights + hidden_units;
    std::span<std::int16_t> own_act;
    std::span<std::int16_t> opp_act;
    if constexpr (WriteAccumulator) {
        own_act = std::span<std::int16_t>(accumulator.values.data(), hidden_units);
        opp_act = std::span<std::int16_t>(accumulator.values.data() + hidden_units, hidden_units);
    }
    accum_persp(own_sums, own_weights, own_act);
    accum_persp(opp_sums, opp_weights, opp_act);

    alignas(32) std::array<std::int32_t, 8> lanes{};
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes.data()), acc);
    std::int64_t dot_sum = acc_tail;
    for (const std::int32_t lane : lanes) {
        dot_sum += lane;
    }
    return scale_v9_output(network, bucket, dot_sum);
}
#endif

template <typename SumT, bool WriteAccumulator = true>
[[nodiscard]] int dispatch_finish_inference_v9_t(const NnueNetwork& network,
                                                 std::span<const SumT> own_sums,
                                                 std::span<const SumT> opp_sums,
                                                 const std::size_t bucket,
                                                 NnueAccumulator& accumulator,
                                                 const bool use_avx2,
                                                 const bool use_simd) noexcept {
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        return finish_inference_v9_avx2_t<SumT, WriteAccumulator>(
            network, own_sums, opp_sums, bucket, accumulator);
    }
#else
    (void)use_avx2;
#endif
#if KOI_NNUE_COMPILED_NEON
    if (use_simd) {
        return finish_inference_v9_neon_t<SumT, WriteAccumulator>(
            network, own_sums, opp_sums, bucket, accumulator);
    }
#else
    (void)use_simd;
#endif
    return finish_inference_v9_scalar_t<SumT, WriteAccumulator>(
        network, own_sums, opp_sums, bucket, accumulator);
}

[[nodiscard]] inline int infer_network_sparse_v9(const NnueNetwork& network,
                                          std::span<const std::uint16_t> own_active,
                                          std::span<const std::uint16_t> opp_active,
                                          const std::size_t bucket,
                                          NnueAccumulator& accumulator,
                                          const bool use_avx2,
                                          const bool use_simd = true) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (!arrays_match_manifest(network) || bucket >= network.manifest.layer_sizes[2]) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }
    accumulator.values.assign(hidden_units * 2U, 0);
    accumulator.bottleneck_values.clear();
    std::vector<std::int32_t> own_sums(hidden_units, 0);
    std::vector<std::int32_t> opp_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, own_active, own_sums);
        accumulate_hidden_sparse_avx2(network, opp_active, opp_sums);
    } else
#endif
    {
        accumulate_hidden_sparse_scalar(network, own_active, own_sums);
        accumulate_hidden_sparse_scalar(network, opp_active, opp_sums);
    }
    return dispatch_finish_inference_v9_t<std::int32_t, true>(
        network, own_sums, opp_sums, bucket, accumulator, use_avx2, use_simd);
}

// v5 inference: both perspective accumulators feed cross-perspective pair
// products p[j] = a_own[j] * a_opp[j], then one CReLU hidden layer and a
// linear head selected by the piece-count bucket.  The scalar path is the
// correctness boundary; the AVX2 helpers below must agree with it exactly.
inline void fill_v5_activations(std::span<const std::int32_t> hidden_sums,
                         std::span<std::int16_t> values) noexcept {
    for (std::size_t hidden = 0; hidden < hidden_sums.size(); ++hidden) {
        values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }
}

inline void compute_cross_pair_products(std::span<const std::int16_t> own,
                                 std::span<const std::int16_t> opp,
                                 std::span<std::int16_t> pairs) noexcept {
    for (std::size_t pair = 0; pair < pairs.size(); ++pair) {
        pairs[pair] = static_cast<std::int16_t>(own[pair] * opp[pair]);
    }
}

// v6 antisymmetric channel: both activations are clipped to [0, 127], so the
// difference fits int16 (-127..127) without saturation.
inline void compute_activation_diff(std::span<const std::int16_t> own,
                             std::span<const std::int16_t> opp,
                             std::span<std::int16_t> diff) noexcept {
    for (std::size_t index = 0; index < diff.size(); ++index) {
        diff[index] = static_cast<std::int16_t>(own[index] - opp[index]);
    }
}

// v6 L1 stage.  The head consumes cross-perspective pair products and the
// activation difference through two int8 matrices; two container layouts share
// the same feature path:
//   * Legacy (l1_diff_shift == 0: v5 containers and pre-split v6 fixtures):
//     `(pair_sum + diff_sum + bias) >> l1_shift`, with the stored int32 bias
//     pre-shifted at the 127 * 2^l1_shift scale.
//   * Split (l1_diff_shift != 0, written by the separate-shift trainer):
//     `(pair_sum >> l1_shift) + (diff_sum >> l1_diff_shift) + bias`, with the
//     stored int32 bias at the plain 127 scale.
// Quantization correspondence for the split layout, with sp = l1_shift and
// sd = l1_diff_shift:
//   l1_weights_q      = round(w * 2^sp / 127)
//   l1_diff_weights_q = round(w * 2^sd)
//   l1_bias_q         = round(bias * 127)
// Both accumulators are int64 (`127 * 127` per pair term) and the arithmetic
// right shifts floor, matching the trainer's numpy int64 `>>`.
inline void l1_activations_scalar(const NnueNetwork& network,
                           std::span<const std::int16_t> pairs,
                           std::span<const std::int16_t> diff,
                           std::span<std::int32_t> activations) noexcept {
    const std::size_t pair_count = pairs.size();
    const bool use_diff = !diff.empty();
    const bool split = use_diff && network.l1_diff_shift > 0;
    for (std::size_t unit = 0; unit < activations.size(); ++unit) {
        const std::int8_t* weights = network.l1_weights.data() + unit * pair_count;
        const std::int8_t* diff_weights = use_diff ?
            network.l1_diff_weights.data() + unit * pair_count : nullptr;
        std::int64_t pair_sum = 0;
        std::int64_t diff_sum = 0;
        for (std::size_t pair = 0; pair < pair_count; ++pair) {
            pair_sum += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
            if (use_diff) {
                diff_sum += static_cast<std::int64_t>(diff_weights[pair]) * diff[pair];
            }
        }
        std::int64_t sum;
        if (split) {
            sum = (pair_sum >> network.l1_shift) +
                (diff_sum >> network.l1_diff_shift) + network.l1_bias[unit];
        } else {
            sum = pair_sum;
            if (use_diff) {
                sum += diff_sum;
            }
            sum += network.l1_bias[unit];
            if (network.l1_shift > 0) {
                sum >>= network.l1_shift;
            }
        }
        activations[unit] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            sum, 0, kKoiNnueClippedReluMaximum));
    }
}

[[nodiscard]] inline int finish_inference_v5_scalar(const NnueNetwork& network,
                                             std::span<const std::int32_t> own_sums,
                                             std::span<const std::int32_t> opp_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden_units;
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    const bool use_diff = is_halfka_threat_v6(network.manifest) ||
        is_halfka_threat_v8(network.manifest);
    accumulator.values.resize(hidden_units);
    accumulator.bottleneck_values.resize(pair_count);
    std::array<std::int16_t, 1536> opp_stack{};
    std::vector<std::int16_t> opp_heap;
    std::span<std::int16_t> opp_values;
    if (hidden_units <= opp_stack.size()) {
        opp_values = std::span<std::int16_t>(opp_stack.data(), hidden_units);
    } else {
        opp_heap.resize(hidden_units);
        opp_values = opp_heap;
    }
    fill_v5_activations(own_sums, accumulator.values);
    fill_v5_activations(opp_sums, opp_values);
    compute_cross_pair_products(accumulator.values, opp_values,
                                accumulator.bottleneck_values);
    std::array<std::int16_t, 1536> diff_stack{};
    std::vector<std::int16_t> diff_heap;
    std::span<std::int16_t> diff_values;
    if (use_diff) {
        if (hidden_units <= diff_stack.size()) {
            diff_values = std::span<std::int16_t>(diff_stack.data(), hidden_units);
        } else {
            diff_heap.resize(hidden_units);
            diff_values = diff_heap;
        }
        compute_activation_diff(accumulator.values, opp_values, diff_values);
    }
    std::array<std::int32_t, kKoiNnueMaximumL1Units> l1_stack{};
    std::vector<std::int32_t> l1_heap;
    std::span<std::int32_t> l1;
    if (l1_units <= l1_stack.size()) {
        l1 = std::span<std::int32_t>(l1_stack.data(), l1_units);
    } else {
        l1_heap.resize(l1_units);
        l1 = l1_heap;
    }
    l1_activations_scalar(network, accumulator.bottleneck_values, diff_values, l1);
    std::int64_t output = network.bottleneck_bias[bucket];
    const std::int8_t* head = network.bottleneck_weights.data() + bucket * l1_units;
    for (std::size_t unit = 0; unit < l1_units; ++unit) {
        output += static_cast<std::int64_t>(head[unit]) * l1[unit];
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

#if KOI_NNUE_COMPILED_AVX2
inline void compute_activation_diff_avx2(std::span<const std::int16_t> own,
                                  std::span<const std::int16_t> opp,
                                  std::span<std::int16_t> diff) noexcept {
    const std::size_t count = diff.size();
    std::size_t index = 0;
    for (; index + 16U <= count; index += 16U) {
        const __m256i own_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(own.data() + index));
        const __m256i opp_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(opp.data() + index));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(diff.data() + index),
                            _mm256_sub_epi16(own_values, opp_values));
    }
    for (; index < count; ++index) {
        diff[index] = static_cast<std::int16_t>(own[index] - opp[index]);
    }
}

inline void compute_cross_pair_products_avx2(std::span<const std::int16_t> own,
                                      std::span<const std::int16_t> opp,
                                      std::span<std::int16_t> pairs) noexcept {
    const std::size_t pair_count = pairs.size();
    std::size_t pair = 0;
    for (; pair + 16U <= pair_count; pair += 16U) {
        const __m256i own_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(own.data() + pair));
        const __m256i opp_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(opp.data() + pair));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(pairs.data() + pair),
                            _mm256_mullo_epi16(own_values, opp_values));
    }
    for (; pair < pair_count; ++pair) {
        pairs[pair] = static_cast<std::int16_t>(own[pair] * opp[pair]);
    }
}

inline void l1_activations_avx2(const NnueNetwork& network,
                         std::span<const std::int16_t> pairs,
                         std::span<const std::int16_t> diff,
                         std::span<std::int32_t> activations) noexcept {
    const std::size_t pair_count = pairs.size();
    const bool use_diff = !diff.empty();
    const bool split = use_diff && network.l1_diff_shift > 0;
    // The pair and diff channels keep separate int32 lane vectors so each lane
    // stays within the v5 worst case: (pair_count / 16 + 1) blocks of two
    // products bounded by 127 * (127 * 127).  The split head cannot merge the
    // channels before the per-channel shifts, and the legacy head merges them
    // only after the lanes are widened to int64 (exact addition).  Fall back to
    // the int64 scalar path when a channel's worst case cannot fit, so the
    // vector path is always equivalent to the scalar reference.
    const std::int64_t lane_worst_case =
        static_cast<std::int64_t>(pair_count / 16U + 1U) * 2 * 128 * (127 * 127);
    if (lane_worst_case > std::numeric_limits<std::int32_t>::max()) {
        l1_activations_scalar(network, pairs, diff, activations);
        return;
    }
    alignas(32) std::array<std::int32_t, 8> lanes{};
    for (std::size_t unit = 0; unit < activations.size(); ++unit) {
        const std::int8_t* weights = network.l1_weights.data() + unit * pair_count;
        const std::int8_t* diff_weights = use_diff ?
            network.l1_diff_weights.data() + unit * pair_count : nullptr;
        __m256i pair_accumulator = _mm256_setzero_si256();
        __m256i diff_accumulator = _mm256_setzero_si256();
        std::size_t pair = 0;
        for (; pair + 16U <= pair_count; pair += 16U) {
            const __m256i pair_values = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(pairs.data() + pair));
            const __m128i weight_bytes = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(weights + pair));
            const __m256i weight_values = _mm256_cvtepi8_epi16(weight_bytes);
            pair_accumulator = _mm256_add_epi32(
                pair_accumulator, _mm256_madd_epi16(pair_values, weight_values));
            if (use_diff) {
                const __m256i diff_values = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(diff.data() + pair));
                const __m128i diff_weight_bytes = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(diff_weights + pair));
                const __m256i diff_weight_values = _mm256_cvtepi8_epi16(diff_weight_bytes);
                diff_accumulator = _mm256_add_epi32(
                    diff_accumulator, _mm256_madd_epi16(diff_values, diff_weight_values));
            }
        }
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes.data()), pair_accumulator);
        std::int64_t pair_sum = 0;
        for (const std::int32_t lane : lanes) {
            pair_sum += lane;
        }
        std::int64_t diff_sum = 0;
        if (use_diff) {
            _mm256_store_si256(reinterpret_cast<__m256i*>(lanes.data()), diff_accumulator);
            for (const std::int32_t lane : lanes) {
                diff_sum += lane;
            }
        }
        for (; pair < pair_count; ++pair) {
            pair_sum += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
            if (use_diff) {
                diff_sum += static_cast<std::int64_t>(diff_weights[pair]) * diff[pair];
            }
        }
        std::int64_t sum;
        if (split) {
            sum = (pair_sum >> network.l1_shift) +
                (diff_sum >> network.l1_diff_shift) + network.l1_bias[unit];
        } else {
            sum = pair_sum;
            if (use_diff) {
                sum += diff_sum;
            }
            sum += network.l1_bias[unit];
            if (network.l1_shift > 0) {
                sum >>= network.l1_shift;
            }
        }
        activations[unit] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            sum, 0, kKoiNnueClippedReluMaximum));
    }
}

[[nodiscard]] inline int finish_inference_v5_avx2(const NnueNetwork& network,
                                           std::span<const std::int32_t> own_sums,
                                           std::span<const std::int32_t> opp_sums,
                                           const std::size_t bucket,
                                           NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden_units;
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    const bool use_diff = is_halfka_threat_v6(network.manifest) ||
        is_halfka_threat_v8(network.manifest);
    accumulator.values.resize(hidden_units);
    accumulator.bottleneck_values.resize(pair_count);
    std::array<std::int16_t, 1536> opp_stack{};
    std::vector<std::int16_t> opp_heap;
    std::span<std::int16_t> opp_values;
    if (hidden_units <= opp_stack.size()) {
        opp_values = std::span<std::int16_t>(opp_stack.data(), hidden_units);
    } else {
        opp_heap.resize(hidden_units);
        opp_values = opp_heap;
    }
    fill_v4_activations_avx2(own_sums, accumulator.values);
    fill_v4_activations_avx2(opp_sums, opp_values);
    compute_cross_pair_products_avx2(accumulator.values, opp_values,
                                     accumulator.bottleneck_values);
    std::array<std::int16_t, 1536> diff_stack{};
    std::vector<std::int16_t> diff_heap;
    std::span<std::int16_t> diff_values;
    if (use_diff) {
        if (hidden_units <= diff_stack.size()) {
            diff_values = std::span<std::int16_t>(diff_stack.data(), hidden_units);
        } else {
            diff_heap.resize(hidden_units);
            diff_values = diff_heap;
        }
        compute_activation_diff_avx2(accumulator.values, opp_values, diff_values);
    }
    std::array<std::int32_t, kKoiNnueMaximumL1Units> l1_stack{};
    std::vector<std::int32_t> l1_heap;
    std::span<std::int32_t> l1;
    if (l1_units <= l1_stack.size()) {
        l1 = std::span<std::int32_t>(l1_stack.data(), l1_units);
    } else {
        l1_heap.resize(l1_units);
        l1 = l1_heap;
    }
    l1_activations_avx2(network, accumulator.bottleneck_values, diff_values, l1);
    std::int64_t output = network.bottleneck_bias[bucket];
    const std::int8_t* head = network.bottleneck_weights.data() + bucket * l1_units;
    for (std::size_t unit = 0; unit < l1_units; ++unit) {
        output += static_cast<std::int64_t>(head[unit]) * l1[unit];
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}
#endif

[[nodiscard]] inline int infer_network_sparse_v5(const NnueNetwork& network,
                                          std::span<const std::uint16_t> own_active,
                                          std::span<const std::uint16_t> opp_active,
                                          const std::size_t bucket,
                                          NnueAccumulator& accumulator,
                                          const bool use_avx2) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (!arrays_match_manifest(network) || bucket >= network.manifest.layer_sizes[2]) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }
    std::vector<std::int32_t> own_sums(hidden_units, 0);
    std::vector<std::int32_t> opp_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, own_active, own_sums);
        accumulate_hidden_sparse_avx2(network, opp_active, opp_sums);
        return finish_inference_v5_avx2(network, own_sums, opp_sums, bucket, accumulator);
    }
#else
    (void)use_avx2;
#endif
    accumulate_hidden_sparse_scalar(network, own_active, own_sums);
    accumulate_hidden_sparse_scalar(network, opp_active, opp_sums);
    return finish_inference_v5_scalar(network, own_sums, opp_sums, bucket, accumulator);
}

#if KOI_NNUE_COMPILED_AVX2
// Applies one feature row to a perspective accumulator with 8 int32 lanes.
// The scalar path accumulates in int64 and clamps to int32; the vector path
// uses int32 lanes and redoes an 8-lane block scalar only when the addition
// would have overflowed (same-sign operands with a changed result sign).
inline void apply_delta_avx2(std::int32_t* values, const std::int16_t* weights,
                      const std::size_t count, const int sign) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    std::size_t index = 0;
    for (; index + 8U <= count; index += 8U) {
        const __m256i current =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values + index));
        __m256i delta = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights + index)));
        if (sign < 0) {
            delta = _mm256_sub_epi32(zero, delta);
        }
        const __m256i sum = _mm256_add_epi32(current, delta);
        const __m256i differing_operands =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(current, delta));
        const __m256i differing_result =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(current, sum));
        const __m256i overflow = _mm256_andnot_si256(differing_operands, differing_result);
        if (_mm256_movemask_epi8(overflow) != 0) {
            for (std::size_t lane = index; lane < index + 8U; ++lane) {
                const std::int64_t updated = static_cast<std::int64_t>(values[lane]) +
                    static_cast<std::int64_t>(sign) * static_cast<std::int64_t>(weights[lane]);
                values[lane] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    updated, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
            continue;
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(values + index), sum);
    }
    for (; index < count; ++index) {
        const std::int64_t updated = static_cast<std::int64_t>(values[index]) +
            static_cast<std::int64_t>(sign) * static_cast<std::int64_t>(weights[index]);
        values[index] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            updated, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    }
}

inline void apply_fused_sub1_add1_avx2(const std::int32_t* parent, std::int32_t* target,
                                const std::int16_t* w_sub0,
                                const std::int16_t* w_add0,
                                const std::size_t count) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    for (std::size_t index = 0; index + 8U <= count; index += 8U) {
        const __m256i cur =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(parent + index));
        const __m256i s0 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_sub0 + index)));
        const __m256i a0 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_add0 + index)));
        const __m256i delta = _mm256_sub_epi32(a0, s0);
        const __m256i sum = _mm256_add_epi32(cur, delta);
        const __m256i differing_operands =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(cur, delta));
        const __m256i differing_result =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(cur, sum));
        const __m256i overflow = _mm256_andnot_si256(differing_operands, differing_result);
        if (_mm256_movemask_epi8(overflow) != 0) {
            for (std::size_t lane = index; lane < index + 8U; ++lane) {
                std::int64_t v = static_cast<std::int64_t>(parent[lane]) - w_sub0[lane];
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += w_add0[lane];
                target[lane] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    v, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
            continue;
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(target + index), sum);
    }
}

inline void apply_fused_sub2_add1_avx2(const std::int32_t* parent, std::int32_t* target,
                                const std::int16_t* w_sub0,
                                const std::int16_t* w_sub1,
                                const std::int16_t* w_add0,
                                const std::size_t count) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    for (std::size_t index = 0; index + 8U <= count; index += 8U) {
        const __m256i cur =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(parent + index));
        const __m256i s0 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_sub0 + index)));
        const __m256i s1 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_sub1 + index)));
        const __m256i a0 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_add0 + index)));
        const __m256i delta = _mm256_sub_epi32(_mm256_sub_epi32(a0, s0), s1);
        const __m256i sum = _mm256_add_epi32(cur, delta);
        const __m256i differing_operands =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(cur, delta));
        const __m256i differing_result =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(cur, sum));
        const __m256i overflow = _mm256_andnot_si256(differing_operands, differing_result);
        if (_mm256_movemask_epi8(overflow) != 0) {
            for (std::size_t lane = index; lane < index + 8U; ++lane) {
                std::int64_t v = static_cast<std::int64_t>(parent[lane]) - w_sub0[lane];
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v -= w_sub1[lane];
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += w_add0[lane];
                target[lane] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    v, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
            continue;
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(target + index), sum);
    }
}

inline void apply_fused_sub2_add2_avx2(const std::int32_t* parent, std::int32_t* target,
                                const std::int16_t* w_sub0,
                                const std::int16_t* w_sub1,
                                const std::int16_t* w_add0,
                                const std::int16_t* w_add1,
                                const std::size_t count) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    for (std::size_t index = 0; index + 8U <= count; index += 8U) {
        const __m256i cur =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(parent + index));
        const __m256i s0 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_sub0 + index)));
        const __m256i s1 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_sub1 + index)));
        const __m256i a0 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_add0 + index)));
        const __m256i a1 = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(w_add1 + index)));
        const __m256i delta =
            _mm256_sub_epi32(_mm256_add_epi32(a0, a1), _mm256_add_epi32(s0, s1));
        const __m256i sum = _mm256_add_epi32(cur, delta);
        const __m256i differing_operands =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(cur, delta));
        const __m256i differing_result =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(cur, sum));
        const __m256i overflow = _mm256_andnot_si256(differing_operands, differing_result);
        if (_mm256_movemask_epi8(overflow) != 0) {
            for (std::size_t lane = index; lane < index + 8U; ++lane) {
                std::int64_t v = static_cast<std::int64_t>(parent[lane]) - w_sub0[lane];
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += w_add0[lane];
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v -= w_sub1[lane];
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += w_add1[lane];
                target[lane] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    v, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
            continue;
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(target + index), sum);
    }
}
#endif

inline void apply_delta_16(std::int16_t* values, const std::int16_t* weights,
                    const std::size_t count, const int sign) noexcept {
    std::size_t index = 0;
#if KOI_NNUE_COMPILED_NEON
    if (sign > 0) {
        for (; index + 16U <= count; index += 16U) {
            vst1q_s16(values + index,
                      vaddq_s16(vld1q_s16(values + index), vld1q_s16(weights + index)));
            vst1q_s16(values + index + 8U,
                      vaddq_s16(vld1q_s16(values + index + 8U), vld1q_s16(weights + index + 8U)));
        }
        for (; index + 8U <= count; index += 8U) {
            vst1q_s16(values + index,
                      vaddq_s16(vld1q_s16(values + index), vld1q_s16(weights + index)));
        }
    } else {
        for (; index + 16U <= count; index += 16U) {
            vst1q_s16(values + index,
                      vsubq_s16(vld1q_s16(values + index), vld1q_s16(weights + index)));
            vst1q_s16(values + index + 8U,
                      vsubq_s16(vld1q_s16(values + index + 8U), vld1q_s16(weights + index + 8U)));
        }
        for (; index + 8U <= count; index += 8U) {
            vst1q_s16(values + index,
                      vsubq_s16(vld1q_s16(values + index), vld1q_s16(weights + index)));
        }
    }
#elif KOI_NNUE_COMPILED_AVX2
    if (avx2_available()) {
        if (sign > 0) {
            for (; index + 16U <= count; index += 16U) {
                const __m256i cur = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(values + index));
                const __m256i w = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(weights + index));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(values + index),
                                    _mm256_add_epi16(cur, w));
            }
        } else {
            for (; index + 16U <= count; index += 16U) {
                const __m256i cur = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(values + index));
                const __m256i w = _mm256_loadu_si256(
                    reinterpret_cast<const __m256i*>(weights + index));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(values + index),
                                    _mm256_sub_epi16(cur, w));
            }
        }
    }
#endif
    if (sign > 0) {
        for (; index < count; ++index) {
            values[index] = static_cast<std::int16_t>(values[index] + weights[index]);
        }
    } else {
        for (; index < count; ++index) {
            values[index] = static_cast<std::int16_t>(values[index] - weights[index]);
        }
    }
}

} // namespace koi::detail
