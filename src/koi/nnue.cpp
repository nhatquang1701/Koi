#include "koi/nnue.hpp"
#include "koi/detail/nnue_manifest.hpp"
#include "koi/detail/nnue_kernels.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#include <utility>

namespace koi {
namespace {

using detail::accumulate_hidden_sparse_16;
using detail::accumulate_hidden_sparse_scalar;
using detail::apply_delta_16;
using detail::avx2_available;
using detail::compute_int16_accumulator_safe;
using detail::dispatch_finish_inference_v9_t;
using detail::finish_inference_v4_dual_scalar;
using detail::finish_inference_v4_dual_scalar_t;
using detail::finish_inference_v4_scalar;
using detail::finish_inference_v4_scalar_t;
using detail::finish_inference_v5_scalar;
using detail::halfka_v4_head_inputs;
using detail::infer_network;
using detail::infer_network_sparse;
using detail::infer_network_sparse_v4;
using detail::infer_network_sparse_v4_dual;
using detail::infer_network_sparse_v5;
using detail::infer_network_sparse_v9;
using detail::is_halfka_dual_v4;
using detail::is_halfka_hm_v9;
using detail::is_halfka_king_bucket_v1;
using detail::is_halfka_threat_v5;
using detail::is_halfka_threat_v6;
using detail::is_halfka_threat_v8;
using detail::output_bucket_for_pieces;
using detail::piece_count_bucket;
using detail::piece_count_of;
using detail::promotion_piece_type;
using detail::uses_halfka_threat_features;

#if KOI_NNUE_COMPILED_AVX2
using detail::accumulate_hidden_sparse_avx2;
using detail::apply_delta_avx2;
using detail::apply_fused_sub1_add1_avx2;
using detail::apply_fused_sub2_add1_avx2;
using detail::apply_fused_sub2_add2_avx2;
using detail::finish_inference_v4_avx2;
using detail::finish_inference_v4_avx2_t;
using detail::finish_inference_v4_dual_avx2;
using detail::finish_inference_v4_dual_avx2_t;
using detail::finish_inference_v5_avx2;
#endif

#if KOI_NNUE_COMPILED_NEON
using detail::finish_inference_v4_dual_neon;
using detail::finish_inference_v4_dual_neon_t;
using detail::finish_inference_v4_neon;
using detail::finish_inference_v4_neon_t;
#endif

[[nodiscard]] std::optional<NnueError> validate_manifest(const NnueManifest& manifest) {
    return detail::validate_nnue_manifest(manifest);
}

[[nodiscard]] bool arrays_match_manifest(const NnueNetwork& network) noexcept {
    return detail::nnue_arrays_match_manifest(network);
}

} // namespace

NnueWorker::NnueWorker(std::shared_ptr<const NnueNetwork> weights)
    : NnueWorker(std::move(weights), false, false) {}

NnueWorker::NnueWorker(std::shared_ptr<const NnueNetwork> weights,
                       const bool int16_safe_known,
                       const bool precomputed_int16_safe)
    : weights_(std::move(weights)),
      int16_safe_known_(int16_safe_known),
      int16_safe_(precomputed_int16_safe) {
    network_usable_ = weights_ && !validate_manifest(weights_->manifest).has_value() &&
        arrays_match_manifest(*weights_);
    if (network_usable_) {
        const std::size_t hidden_units = weights_->manifest.layer_sizes[1];
        const bool threat = uses_halfka_threat_features(weights_->manifest);
        const bool halfka_v4 = is_halfka_king_bucket_v1(weights_->manifest);
        const bool dual_v4 = is_halfka_dual_v4(weights_->manifest);
        const bool hm_v9 = is_halfka_hm_v9(weights_->manifest);
        supports_incremental_ = halfka_v4 || hm_v9 || threat;
        threat_features_ = threat;
        dual_v4_ = dual_v4;
        halfka_hm_v9_ = hm_v9;
        accumulator_.values.resize((dual_v4 || hm_v9) ? hidden_units * 2U : hidden_units);
        accumulator_.bottleneck_values.resize(threat ? hidden_units :
            (halfka_v4 ? halfka_v4_head_inputs(weights_->manifest) :
            (hm_v9 ? std::size_t{0} : weights_->manifest.layer_sizes[2])));
    }
}

int NnueWorker::evaluate(const EvaluationFeatures& features, const Color perspective,
                         const NnueInferencePath path) {
    if (!network_usable_) {
        return 0;
    }
    const bool use_hm_v9 = is_halfka_hm_v9(weights_->manifest);
    const bool use_halfka = is_halfka_king_bucket_v1(weights_->manifest);
    const bool use_dual_halfka = is_halfka_dual_v4(weights_->manifest);
    const bool use_threat = uses_halfka_threat_features(weights_->manifest);
    const bool use_v2 = weights_->manifest.feature_set ==
        kKoiNnuePieceSquareKingPawnV2FeatureSet;
    const bool use_avx2 = path == NnueInferencePath::avx2_compatible ? avx2_available() :
        (path == NnueInferencePath::automatic && avx2_available());
    const bool use_simd = path != NnueInferencePath::scalar;
    int score = 0;
    if (use_hm_v9) {
        const Color mover = features.position.side_to_move;
        const NnueSparseFeaturesV4 own =
            EvaluationFeatureExtractor::encode_sparse_v9(features, mover);
        const NnueSparseFeaturesV4 opp =
            EvaluationFeatureExtractor::encode_sparse_v9(features, opposite(mover));
        score = infer_network_sparse_v9(
            *weights_, std::span<const std::uint16_t>(own.indices.data(), own.count),
            std::span<const std::uint16_t>(opp.indices.data(), opp.count),
            piece_count_bucket(features), accumulator_, use_avx2, use_simd);
    } else if (use_threat) {
        // Both perspectives feed the head: own is the side to move, opponent
        // is the other side.  The sign flip below keeps the documented
        // "raw score is mover-relative" contract.  Version 6 adds the
        // antisymmetric activation difference inside the L1 stage.
        const Color mover = features.position.side_to_move;
        const NnueSparseFeaturesV5 own =
            EvaluationFeatureExtractor::encode_sparse_v5(features, mover);
        const NnueSparseFeaturesV5 opp = EvaluationFeatureExtractor::encode_sparse_v5(
            features, opposite(mover));
        score = infer_network_sparse_v5(
            *weights_, std::span<const std::uint16_t>(own.indices.data(), own.count),
            std::span<const std::uint16_t>(opp.indices.data(), opp.count),
            piece_count_bucket(features), accumulator_, use_avx2);
    } else if (use_dual_halfka) {
        const Color mover = features.position.side_to_move;
        const NnueSparseFeaturesV4 own =
            EvaluationFeatureExtractor::encode_sparse_v4(features, mover);
        const NnueSparseFeaturesV4 opp =
            EvaluationFeatureExtractor::encode_sparse_v4(features, opposite(mover));
        score = infer_network_sparse_v4_dual(
            *weights_, std::span<const std::uint16_t>(own.indices.data(), own.count),
            std::span<const std::uint16_t>(opp.indices.data(), opp.count),
            piece_count_bucket(features), accumulator_, use_avx2, use_simd);
    } else if (use_halfka) {
        const NnueSparseFeaturesV4 sparse =
            EvaluationFeatureExtractor::encode_sparse_v4(features);
        score = infer_network_sparse_v4(
            *weights_, std::span<const std::uint16_t>(sparse.indices.data(), sparse.count),
            piece_count_bucket(features), accumulator_, use_avx2, use_simd);
    } else if (use_v2) {
        const NnueSparseFeatures sparse =
            EvaluationFeatureExtractor::encode_sparse_v2(features);
        score = infer_network_sparse(
            *weights_, std::span<const std::uint16_t>(sparse.indices.data(), sparse.count),
            accumulator_, use_avx2);
    } else {
        const auto encoded = EvaluationFeatureExtractor::encode_piece_square_v1(features);
        score = infer_network(*weights_, encoded, accumulator_, use_avx2);
    }
    // The encoder expresses features from the side to move's perspective, so
    // the raw network score is already relative to the mover. Flip it only
    // when the caller asked for the opposite perspective.
    const Color mover = features.position.side_to_move;
    return perspective == mover ? score : -score;
}

#if defined(__clang__) || defined(__GNUC__)
#define KOI_NNUE_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define KOI_NNUE_NOINLINE __declspec(noinline)
#else
#define KOI_NNUE_NOINLINE
#endif

template <bool WriteAccumulator>
KOI_NNUE_NOINLINE int NnueWorker::evaluate_state_fallback_impl(
    const GameState& state, const Color perspective,
    const NnueInferencePath path) {
    if (!supports_incremental()) {
        return evaluate(EvaluationFeatureExtractor::extract(state), perspective, path);
    }
    if (!storage_ready_) {
        ensure_incremental_storage();
        if (int16_safe_) {
            return evaluate_state_impl<WriteAccumulator>(state, perspective, path);
        }
    }
    const bool threat = threat_features_;
    const Color mover = state.side_to_move();
    const std::size_t mover_index = mover == Color::white ? 0U : 1U;
    const std::size_t opponent_index = 1U - mover_index;
    const std::uint64_t key = state.position_key();
    const bool use_avx2 = path == NnueInferencePath::avx2_compatible ? avx2_available() :
        (path == NnueInferencePath::automatic && avx2_available());
    (void)use_avx2;
    const bool use_simd = path != NnueInferencePath::scalar;
    (void)use_simd;
    std::size_t pieces = 0;

    std::span<const std::int32_t> mover_sums;
    std::span<const std::int32_t> opponent_sums;
    if (slot_cursor_ >= 0 &&
        slot_valid_[static_cast<std::size_t>(slot_cursor_)] != 0 &&
        slot_keys_[static_cast<std::size_t>(slot_cursor_)] == key) {
        const std::size_t slot = static_cast<std::size_t>(slot_cursor_);
        if (!threat) {
            materialize_slot_perspective_v4(state, mover_index, slot);
            if (dual_v4_ || halfka_hm_v9_) {
                materialize_slot_perspective_v4(state, opponent_index, slot);
            }
        }
        mover_sums = std::span<const std::int32_t>(
            slot_values_[mover_index].data() + slot * hidden_units_, hidden_units_);
        opponent_sums = std::span<const std::int32_t>(
            slot_values_[opponent_index].data() + slot * hidden_units_, hidden_units_);
        pieces = slot_pieces_[slot];
    } else if (scratch_valid_ && scratch_key_ == key) {
        mover_sums = std::span<const std::int32_t>(scratch_values_[mover_index]);
        opponent_sums = std::span<const std::int32_t>(scratch_values_[opponent_index]);
        pieces = scratch_pieces_;
    } else {
        if (!threat) {
            for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
                const Color real = index == 0U ? Color::white : Color::black;
                const std::size_t color_idx = real == Color::white ? 0U : 1U;
                const std::uint64_t king_bb =
                    state.piece_bitboards()[color_idx][static_cast<std::size_t>(PieceType::king)];
                const Square king_sq = king_bb != 0 ?
                    Square::from_index(static_cast<std::uint8_t>(std::countr_zero(king_bb))) :
                    Square{};
                const std::size_t bucket = halfka_hm_v9_ ?
                    EvaluationFeatureExtractor::halfka_hm_v2_finny_slot_for_square(king_sq, real) :
                    EvaluationFeatureExtractor::halfka_king_bucket_for_square(king_sq, real);
                refresh_finny_bucket_v4(state, real, bucket, scratch_values_[index].data());
                scratch_buckets_[index] = static_cast<std::uint8_t>(bucket);
            }
            scratch_pieces_ = static_cast<std::uint8_t>(state.piece_count());
        } else {
            // Full rebuild: the only path that materializes the feature view.
            const EvaluationFeatures features = EvaluationFeatureExtractor::extract(state);
            for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
                const Color real = index == 0U ? Color::white : Color::black;
                const NnueSparseFeaturesV5 sparse =
                    EvaluationFeatureExtractor::encode_sparse_v5(features, real);
                const std::span<const std::uint16_t> active(
                    sparse.indices.data(), sparse.count);
#if KOI_NNUE_COMPILED_AVX2
                if (use_avx2) {
                    accumulate_hidden_sparse_avx2(*weights_, active,
                                                  scratch_values_[index]);
                } else
#endif
                {
                    accumulate_hidden_sparse_scalar(*weights_, active,
                                                    scratch_values_[index]);
                }
                const NnueSparseFeaturesThreatV1 threats =
                    EvaluationFeatureExtractor::encode_sparse_threat_v1(features, real);
                std::copy_n(threats.indices.data(), threats.count,
                            scratch_threats_[index].data());
                scratch_threat_counts_[index] = static_cast<std::uint8_t>(threats.count);
                scratch_buckets_[index] = static_cast<std::uint8_t>(
                    EvaluationFeatureExtractor::halfka_king_bucket_for(features, real));
            }
            scratch_pieces_ = static_cast<std::uint8_t>(piece_count_of(features));
        }
        scratch_key_ = key;
        scratch_valid_ = true;
        ++incremental_fallback_count_;
        mover_sums = std::span<const std::int32_t>(scratch_values_[mover_index]);
        opponent_sums = std::span<const std::int32_t>(scratch_values_[opponent_index]);
        pieces = scratch_pieces_;
    }
    const std::size_t bucket = output_bucket_for_pieces(pieces);
    int score = 0;
    if (threat) {
#if KOI_NNUE_COMPILED_AVX2
        if (use_avx2) {
            score = finish_inference_v5_avx2(*weights_, mover_sums, opponent_sums,
                                             bucket, accumulator_);
        } else
#endif
        {
            score = finish_inference_v5_scalar(*weights_, mover_sums, opponent_sums,
                                               bucket, accumulator_);
        }
    } else if (halfka_hm_v9_) {
        score = dispatch_finish_inference_v9_t<std::int32_t, WriteAccumulator>(
            *weights_, mover_sums, opponent_sums, bucket, accumulator_, use_avx2, use_simd);
    } else if (dual_v4_) {
#if KOI_NNUE_COMPILED_AVX2
        if (use_avx2) {
            score = finish_inference_v4_dual_avx2(*weights_, mover_sums, opponent_sums,
                                                  bucket, accumulator_);
        } else
#endif
#if KOI_NNUE_COMPILED_NEON
        if (use_simd) {
            score = finish_inference_v4_dual_neon(*weights_, mover_sums, opponent_sums,
                                                  bucket, accumulator_);
        } else
#endif
        {
            score = finish_inference_v4_dual_scalar(*weights_, mover_sums, opponent_sums,
                                                    bucket, accumulator_);
        }
    } else {
#if KOI_NNUE_COMPILED_AVX2
        if (use_avx2) {
            score = finish_inference_v4_avx2(*weights_, mover_sums, bucket, accumulator_);
        } else
#endif
#if KOI_NNUE_COMPILED_NEON
        if (use_simd) {
            score = finish_inference_v4_neon(*weights_, mover_sums, bucket, accumulator_);
        } else
#endif
        {
            score = finish_inference_v4_scalar(*weights_, mover_sums, bucket, accumulator_);
        }
    }
    return perspective == mover ? score : -score;
}

#undef KOI_NNUE_NOINLINE

template <bool WriteAccumulator>
int NnueWorker::evaluate_state_impl(const GameState& state, const Color perspective,
                                    const NnueInferencePath path) {
    if (!supports_incremental() || !storage_ready_ || !int16_safe_) [[unlikely]] {
        return evaluate_state_fallback_impl<WriteAccumulator>(state, perspective, path);
    }
    const Color mover = state.side_to_move();
    const std::size_t mover_index = mover == Color::white ? 0U : 1U;
    const std::size_t opponent_index = 1U - mover_index;
    const std::uint64_t key = state.position_key();
    const bool use_avx2 = path == NnueInferencePath::avx2_compatible ? avx2_available() :
        (path == NnueInferencePath::automatic && avx2_available());
    (void)use_avx2;
    const bool use_simd = path != NnueInferencePath::scalar;
    (void)use_simd;
    std::size_t pieces = 0;
    std::span<const std::int16_t> mover_sums_16;
    std::span<const std::int16_t> opponent_sums_16;
    if (slot_cursor_ >= 0 &&
        slot_valid_[static_cast<std::size_t>(slot_cursor_)] != 0 &&
        slot_keys_[static_cast<std::size_t>(slot_cursor_)] == key) {
        const std::size_t slot = static_cast<std::size_t>(slot_cursor_);
        if (slot_computed_[mover_index][slot] == 0) {
            materialize_slot_perspective_v4_impl<std::int16_t>(state, mover_index, slot);
        }
        if ((dual_v4_ || halfka_hm_v9_) && slot_computed_[opponent_index][slot] == 0) {
            materialize_slot_perspective_v4_impl<std::int16_t>(state, opponent_index, slot);
        }
        mover_sums_16 = std::span<const std::int16_t>(
            slot_values_16_[mover_index].data() + slot * hidden_units_, hidden_units_);
        opponent_sums_16 = std::span<const std::int16_t>(
            slot_values_16_[opponent_index].data() + slot * hidden_units_, hidden_units_);
        pieces = slot_pieces_[slot];
    } else if (scratch_valid_ && scratch_key_ == key) {
        mover_sums_16 = std::span<const std::int16_t>(scratch_values_16_[mover_index]);
        opponent_sums_16 = std::span<const std::int16_t>(scratch_values_16_[opponent_index]);
        pieces = scratch_pieces_;
    } else {
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            const Color real = index == 0U ? Color::white : Color::black;
            const std::size_t color_idx = real == Color::white ? 0U : 1U;
            const std::uint64_t king_bb =
                state.piece_bitboards()[color_idx][static_cast<std::size_t>(PieceType::king)];
            const Square king_sq = king_bb != 0 ?
                Square::from_index(static_cast<std::uint8_t>(std::countr_zero(king_bb))) :
                Square{};
            const std::size_t bucket_idx = halfka_hm_v9_ ?
                EvaluationFeatureExtractor::halfka_hm_v2_finny_slot_for_square(king_sq, real) :
                EvaluationFeatureExtractor::halfka_king_bucket_for_square(king_sq, real);
            refresh_finny_bucket_v4_impl<std::int16_t>(
                state, real, bucket_idx, scratch_values_16_[index].data());
            scratch_buckets_[index] = static_cast<std::uint8_t>(bucket_idx);
        }
        scratch_pieces_ = static_cast<std::uint8_t>(state.piece_count());
        scratch_key_ = key;
        scratch_valid_ = true;
        ++incremental_fallback_count_;
        mover_sums_16 = std::span<const std::int16_t>(scratch_values_16_[mover_index]);
        opponent_sums_16 = std::span<const std::int16_t>(scratch_values_16_[opponent_index]);
        pieces = scratch_pieces_;
    }
    const std::size_t bucket = output_bucket_for_pieces(pieces);
    int score = 0;
    if (halfka_hm_v9_) {
        score = dispatch_finish_inference_v9_t<std::int16_t, WriteAccumulator>(
            *weights_, mover_sums_16, opponent_sums_16, bucket, accumulator_, use_avx2, use_simd);
    } else if (dual_v4_) {
#if KOI_NNUE_COMPILED_AVX2
        if (use_avx2) {
            score = finish_inference_v4_dual_avx2_t<std::int16_t>(
                *weights_, mover_sums_16, opponent_sums_16, bucket, accumulator_);
        } else
#endif
#if KOI_NNUE_COMPILED_NEON
        if (use_simd) {
            score = finish_inference_v4_dual_neon_t<std::int16_t, WriteAccumulator>(
                *weights_, mover_sums_16, opponent_sums_16, bucket, accumulator_);
        } else
#endif
        {
            score = finish_inference_v4_dual_scalar_t<std::int16_t>(
                *weights_, mover_sums_16, opponent_sums_16, bucket, accumulator_);
        }
    } else {
#if KOI_NNUE_COMPILED_AVX2
        if (use_avx2) {
            score = finish_inference_v4_avx2_t<std::int16_t>(
                *weights_, mover_sums_16, bucket, accumulator_);
        } else
#endif
#if KOI_NNUE_COMPILED_NEON
        if (use_simd) {
            score = finish_inference_v4_neon_t<std::int16_t, WriteAccumulator>(
                *weights_, mover_sums_16, bucket, accumulator_);
        } else
#endif
        {
            score = finish_inference_v4_scalar_t<std::int16_t>(
                *weights_, mover_sums_16, bucket, accumulator_);
        }
    }
    return perspective == mover ? score : -score;
}

int NnueWorker::evaluate(const GameState& state, const Color perspective,
                         const NnueInferencePath path) {
    return evaluate_state_impl<true>(state, perspective, path);
}

int NnueWorker::evaluate_score_only(const GameState& state, const Color perspective,
                                    const NnueInferencePath path) {
    return evaluate_state_impl<false>(state, perspective, path);
}

void NnueWorker::ensure_incremental_storage() {
    if (storage_ready_) {
        return;
    }
    static_assert(kThreatListCapacity ==
                      sizeof(NnueSparseFeaturesThreatV1::indices) /
                          sizeof(std::uint16_t),
                  "worker threat list capacity must match the encoder capacity");
    hidden_units_ = weights_->manifest.layer_sizes[1];
    threat_features_ = uses_halfka_threat_features(weights_->manifest);
    dual_v4_ = is_halfka_dual_v4(weights_->manifest);
    halfka_hm_v9_ = is_halfka_hm_v9(weights_->manifest);
    if (!int16_safe_known_) {
        int16_safe_ = compute_int16_accumulator_safe(*weights_);
        int16_safe_known_ = true;
    } else if (threat_features_) {
        int16_safe_ = false;
    }
    const std::size_t finny_slots =
        halfka_hm_v9_ ? kNnueFinnyBucketCapacity : kNnueKingBucketCount;
    const std::size_t slot_entries = kIncrementalSlotCount * hidden_units_;
    for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
        if (int16_safe_) {
            slot_values_16_[index].assign(slot_entries, 0);
            scratch_values_16_[index].assign(hidden_units_, 0);
            finny_values_16_[index].assign(finny_slots * hidden_units_, 0);
            finny_valid_[index].fill(0);
        } else {
            slot_values_[index].assign(slot_entries, 0);
            scratch_values_[index].assign(hidden_units_, 0);
            if (!threat_features_) {
                finny_values_[index].assign(finny_slots * hidden_units_, 0);
                finny_valid_[index].fill(0);
            }
        }
        slot_buckets_[index].assign(kIncrementalSlotCount, 0);
        slot_computed_[index].assign(kIncrementalSlotCount, 0);
        if (threat_features_) {
            slot_threats_[index].assign(kIncrementalSlotCount * kThreatListCapacity, 0);
            slot_threat_counts_[index].assign(kIncrementalSlotCount, 0);
            scratch_threats_[index].assign(kThreatListCapacity, 0);
        }
    }
    if (!threat_features_) {
        slot_pending_move_.assign(kIncrementalSlotCount, PendingMoveV4{});
    }
    slot_keys_.assign(kIncrementalSlotCount, 0);
    slot_valid_.assign(kIncrementalSlotCount, 0);
    slot_pieces_.assign(kIncrementalSlotCount, 0);
    // All active piece counts (<= 32) plus up to kIncrementalSlotCount plies of
    // signed int16 deltas stay strictly within int32 range whenever every bias
    // lies within +/- 1e9, letting hot fused-delta loops skip per-element
    // clamping and auto-vectorize cleanly.
    overflow_safe_ = true;
    for (const std::int32_t bias : weights_->hidden_bias) {
        if (bias < -1'000'000'000 || bias > 1'000'000'000) {
            overflow_safe_ = false;
            break;
        }
    }
    storage_ready_ = true;
}

void NnueWorker::copy_slot_forward(const std::size_t from_slot,
                                   const std::size_t to_slot) {
    for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
        std::copy_n(slot_values_[index].data() + from_slot * hidden_units_,
                    hidden_units_,
                    slot_values_[index].data() + to_slot * hidden_units_);
        slot_buckets_[index][to_slot] = slot_buckets_[index][from_slot];
        if (threat_features_) {
            slot_threat_counts_[index][to_slot] = slot_threat_counts_[index][from_slot];
            std::copy_n(slot_threats_[index].data() + from_slot * kThreatListCapacity,
                        kThreatListCapacity,
                        slot_threats_[index].data() + to_slot * kThreatListCapacity);
        }
    }
    slot_pieces_[to_slot] = slot_pieces_[from_slot];
}

void NnueWorker::refresh_slot_perspective(const EvaluationFeatures& features,
                                          const Color perspective,
                                          const std::size_t slot) {
    const std::size_t index = perspective == Color::white ? 0U : 1U;
    // Write directly into the destination slot: accumulate_hidden_sparse_*
    // initializes every element from hidden_bias before summing active rows,
    // avoiding both an intermediate zeroing pass and a copy per king-bucket
    // crossing.
    const std::span<std::int32_t> sums(
        slot_values_[index].data() + slot * hidden_units_, hidden_units_);
    if (threat_features_) {
        const NnueSparseFeaturesV5 sparse =
            EvaluationFeatureExtractor::encode_sparse_v5(features, perspective);
        const std::span<const std::uint16_t> active(
            sparse.indices.data(), sparse.count);
#if KOI_NNUE_COMPILED_AVX2
        if (avx2_available()) {
            accumulate_hidden_sparse_avx2(*weights_, active, sums);
        } else
#endif
        {
            accumulate_hidden_sparse_scalar(*weights_, active, sums);
        }
        store_threat_list(index, slot,
                          EvaluationFeatureExtractor::encode_sparse_threat_v1(
                              features, perspective));
    } else {
        const NnueSparseFeaturesV4 sparse =
            EvaluationFeatureExtractor::encode_sparse_v4(features, perspective);
        const std::span<const std::uint16_t> active(
            sparse.indices.data(), sparse.count);
#if KOI_NNUE_COMPILED_AVX2
        if (avx2_available()) {
            accumulate_hidden_sparse_avx2(*weights_, active, sums);
        } else
#endif
        {
            accumulate_hidden_sparse_scalar(*weights_, active, sums);
        }
    }
    slot_buckets_[index][slot] = static_cast<std::uint8_t>(
        EvaluationFeatureExtractor::halfka_king_bucket_for(features, perspective));
    slot_pieces_[slot] = static_cast<std::uint8_t>(piece_count_of(features));
}

template <typename SumT>
void NnueWorker::refresh_finny_bucket_v4_impl(const GameState& state,
                                              const Color perspective,
                                              const std::size_t bucket,
                                              SumT* const target_sums) {
    const std::size_t index = perspective == Color::white ? 0U : 1U;
    const auto& current_bbs = state.piece_bitboards();
    SumT* cached_sums = nullptr;
    if constexpr (std::is_same_v<SumT, std::int16_t>) {
        cached_sums = finny_values_16_[index].data() + bucket * hidden_units_;
    } else {
        cached_sums = finny_values_[index].data() + bucket * hidden_units_;
    }

    const std::size_t feat_bucket = halfka_hm_v9_ ? (bucket >> 1U) : bucket;
    const std::size_t base = feat_bucket * kNnuePieceSquareV1FeatureCount;
    const std::size_t sq_xor = (perspective == Color::white ? 0U : 56U) ^
        (halfka_hm_v9_ && (bucket & 1U) != 0U ? 7U : 0U);

    if (finny_valid_[index][bucket] == 0) {
        std::array<std::uint16_t, kNnueSparseFeatureCapacityV4> active_buf{};
        std::size_t active_count = 0;
        const std::size_t own_c = perspective == Color::white ? 0U : 1U;
        const std::size_t opp_c = 1U - own_c;
        const std::array<std::size_t, 2> color_order{own_c, opp_c};
        for (std::size_t plane_group = 0; plane_group < 2U; ++plane_group) {
            const std::size_t c = color_order[plane_group];
            for (std::size_t pt = 1U; pt <= 6U; ++pt) {
                const std::size_t plane = plane_group * 6U + (pt - 1U);
                const std::size_t plane_base = base + plane * 64U;
                std::uint64_t bb = current_bbs[c][pt];
                while (bb != 0 && active_count < active_buf.size()) {
                    const std::size_t sq = static_cast<std::size_t>(std::countr_zero(bb));
                    bb &= bb - 1U;
                    active_buf[active_count++] =
                        static_cast<std::uint16_t>(plane_base + (sq ^ sq_xor));
                }
            }
        }
        const std::span<const std::uint16_t> active(active_buf.data(), active_count);
        const std::span<SumT> cached_span(cached_sums, hidden_units_);
        if constexpr (std::is_same_v<SumT, std::int16_t>) {
            accumulate_hidden_sparse_16(*weights_, active, cached_span);
        } else {
#if KOI_NNUE_COMPILED_AVX2
            if (avx2_available()) {
                accumulate_hidden_sparse_avx2(*weights_, active, cached_span);
            } else
#endif
            {
                accumulate_hidden_sparse_scalar(*weights_, active, cached_span);
            }
        }
        finny_bitboards_[index][bucket] = current_bbs;
        finny_valid_[index][bucket] = 1;
        std::copy_n(cached_sums, hidden_units_, target_sums);
        return;
    }

    auto& cached_bbs = finny_bitboards_[index][bucket];
    std::array<std::uint16_t, 64> removed{};
    std::array<std::uint16_t, 64> added{};
    std::size_t removed_count = 0;
    std::size_t added_count = 0;
    const std::size_t own_c = perspective == Color::white ? 0U : 1U;
    const std::size_t opp_c = 1U - own_c;
    const std::array<std::size_t, 2> color_order{own_c, opp_c};
    for (std::size_t plane_group = 0; plane_group < 2U; ++plane_group) {
        const std::size_t c = color_order[plane_group];
        for (std::size_t pt = 1U; pt <= 6U; ++pt) {
            const std::uint64_t old_bb = cached_bbs[c][pt];
            const std::uint64_t new_bb = current_bbs[c][pt];
            if (old_bb == new_bb) {
                continue;
            }
            cached_bbs[c][pt] = new_bb;
            const std::size_t plane = plane_group * 6U + (pt - 1U);
            const std::size_t plane_base = base + plane * 64U;
            std::uint64_t rem_bb = old_bb & ~new_bb;
            std::uint64_t add_bb = new_bb & ~old_bb;
            while (rem_bb != 0 && removed_count < removed.size()) {
                const std::size_t sq = static_cast<std::size_t>(std::countr_zero(rem_bb));
                rem_bb &= rem_bb - 1U;
                removed[removed_count++] =
                    static_cast<std::uint16_t>(plane_base + (sq ^ sq_xor));
            }
            while (add_bb != 0 && added_count < added.size()) {
                const std::size_t sq = static_cast<std::size_t>(std::countr_zero(add_bb));
                add_bb &= add_bb - 1U;
                added[added_count++] =
                    static_cast<std::uint16_t>(plane_base + (sq ^ sq_xor));
            }
        }
    }

    if ((removed_count == 1U && added_count == 1U) ||
        (removed_count == 2U && added_count == 1U) ||
        (removed_count == 2U && added_count == 2U)) {
        apply_fused_perspective_deltas(
            cached_sums, cached_sums,
            std::span<const std::uint16_t>(removed.data(), removed_count),
            std::span<const std::uint16_t>(added.data(), added_count));
        std::copy_n(cached_sums, hidden_units_, target_sums);
        return;
    }

    const std::int16_t* const w_base = weights_->feature_weights.data();
    const std::size_t count = hidden_units_;
    std::size_t pair_idx = 0;
    for (; pair_idx < std::min(removed_count, added_count); ++pair_idx) {
        const std::uint16_t r_feat = removed[pair_idx];
        const std::uint16_t a_feat = added[pair_idx];
        apply_fused_perspective_deltas(
            cached_sums, cached_sums,
            std::span<const std::uint16_t>(&r_feat, 1U),
            std::span<const std::uint16_t>(&a_feat, 1U));
    }
    for (std::size_t r = pair_idx; r < removed_count; ++r) {
        const std::int16_t* const w =
            w_base + static_cast<std::size_t>(removed[r]) * count;
        if constexpr (std::is_same_v<SumT, std::int16_t>) {
            apply_delta_16(cached_sums, w, count, -1);
        } else {
#if KOI_NNUE_COMPILED_AVX2
            if (avx2_available() && count % 16U == 0U) {
                apply_delta_avx2(cached_sums, w, count, -1);
                continue;
            }
#endif
            if (overflow_safe_) {
                for (std::size_t hidden = 0; hidden < count; ++hidden) {
                    cached_sums[hidden] -= w[hidden];
                }
            } else {
                for (std::size_t hidden = 0; hidden < count; ++hidden) {
                    const std::int64_t updated =
                        static_cast<std::int64_t>(cached_sums[hidden]) - w[hidden];
                    cached_sums[hidden] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                        updated, std::numeric_limits<std::int32_t>::min(),
                        std::numeric_limits<std::int32_t>::max()));
                }
            }
        }
    }
    for (std::size_t a = pair_idx; a < added_count; ++a) {
        const std::int16_t* const w =
            w_base + static_cast<std::size_t>(added[a]) * count;
        if constexpr (std::is_same_v<SumT, std::int16_t>) {
            apply_delta_16(cached_sums, w, count, 1);
        } else {
#if KOI_NNUE_COMPILED_AVX2
            if (avx2_available() && count % 16U == 0U) {
                apply_delta_avx2(cached_sums, w, count, 1);
                continue;
            }
#endif
            if (overflow_safe_) {
                for (std::size_t hidden = 0; hidden < count; ++hidden) {
                    cached_sums[hidden] += w[hidden];
                }
            } else {
                for (std::size_t hidden = 0; hidden < count; ++hidden) {
                    const std::int64_t updated =
                        static_cast<std::int64_t>(cached_sums[hidden]) + w[hidden];
                    cached_sums[hidden] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                        updated, std::numeric_limits<std::int32_t>::min(),
                        std::numeric_limits<std::int32_t>::max()));
                }
            }
        }
    }
    std::copy_n(cached_sums, hidden_units_, target_sums);
}

void NnueWorker::refresh_finny_bucket_v4(const GameState& state,
                                         const Color perspective,
                                         const std::size_t bucket,
                                         std::int32_t* const target_sums) {
    refresh_finny_bucket_v4_impl<std::int32_t>(state, perspective, bucket, target_sums);
}

void NnueWorker::refresh_slot_perspective_v4_finny(const GameState& state,
                                                   const Color perspective,
                                                   const std::size_t slot) {
    const std::size_t index = perspective == Color::white ? 0U : 1U;
    const std::uint64_t king_bb =
        state.piece_bitboards()[index][static_cast<std::size_t>(PieceType::king)];
    const Square king_sq = king_bb != 0 ?
        Square::from_index(static_cast<std::uint8_t>(std::countr_zero(king_bb))) :
        Square{};
    const std::size_t bucket = halfka_hm_v9_ ?
        EvaluationFeatureExtractor::halfka_hm_v2_finny_slot_for_square(king_sq, perspective) :
        EvaluationFeatureExtractor::halfka_king_bucket_for_square(king_sq, perspective);
    refresh_finny_bucket_v4(
        state, perspective, bucket,
        slot_values_[index].data() + slot * hidden_units_);
    slot_buckets_[index][slot] = static_cast<std::uint8_t>(bucket);
    slot_pieces_[slot] = static_cast<std::uint8_t>(state.piece_count());
}

void NnueWorker::apply_feature_delta(const std::size_t perspective,
                                     const std::size_t slot,
                                     const std::uint16_t index, const int sign) {
    std::int32_t* values = slot_values_[perspective].data() + slot * hidden_units_;
    const std::int16_t* weights =
        weights_->feature_weights.data() +
        static_cast<std::size_t>(index) * hidden_units_;
#if KOI_NNUE_COMPILED_AVX2
    // The vector path reproduces the scalar int64-accumulate/int32-clamp
    // semantics exactly, including the per-block overflow fallback, so both
    // paths agree on every input.
    if (avx2_available() && hidden_units_ % 16U == 0U) {
        apply_delta_avx2(values, weights, hidden_units_, sign);
        return;
    }
#endif
    for (std::size_t hidden = 0; hidden < hidden_units_; ++hidden) {
        const std::int64_t updated = static_cast<std::int64_t>(values[hidden]) +
            static_cast<std::int64_t>(sign) * static_cast<std::int64_t>(weights[hidden]);
        values[hidden] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            updated, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    }
}

void NnueWorker::apply_fused_perspective_deltas(
    const std::int32_t* const parent_values,
    std::int32_t* const target_values,
    const std::span<const std::uint16_t> removed,
    const std::span<const std::uint16_t> added) {
    const std::int16_t* const base = weights_->feature_weights.data();
    const std::size_t count = hidden_units_;
    if (removed.size() == 1U && added.size() == 1U) {
        const std::int16_t* const w_sub0 =
            base + static_cast<std::size_t>(removed[0]) * count;
        const std::int16_t* const w_add0 =
            base + static_cast<std::size_t>(added[0]) * count;
#if KOI_NNUE_COMPILED_AVX2
        if (avx2_available() && count % 16U == 0U) {
            apply_fused_sub1_add1_avx2(parent_values, target_values, w_sub0, w_add0, count);
            return;
        }
#endif
        if (overflow_safe_) {
            for (std::size_t hidden = 0; hidden < count; ++hidden) {
                target_values[hidden] =
                    parent_values[hidden] - w_sub0[hidden] + w_add0[hidden];
            }
            return;
        }
        constexpr std::int32_t kSafeLimit =
            std::numeric_limits<std::int32_t>::max() - 131072;
        for (std::size_t hidden = 0; hidden < count; ++hidden) {
            const std::int32_t p = parent_values[hidden];
            const std::int32_t s0 = w_sub0[hidden];
            const std::int32_t a0 = w_add0[hidden];
            if (p > -kSafeLimit && p < kSafeLimit) [[likely]] {
                target_values[hidden] = p - s0 + a0;
            } else {
                std::int64_t v = static_cast<std::int64_t>(p) - s0;
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += a0;
                target_values[hidden] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    v, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
        }
        return;
    }
    if (removed.size() == 2U && added.size() == 1U) {
        const std::int16_t* const w_sub0 =
            base + static_cast<std::size_t>(removed[0]) * count;
        const std::int16_t* const w_sub1 =
            base + static_cast<std::size_t>(removed[1]) * count;
        const std::int16_t* const w_add0 =
            base + static_cast<std::size_t>(added[0]) * count;
#if KOI_NNUE_COMPILED_AVX2
        if (avx2_available() && count % 16U == 0U) {
            apply_fused_sub2_add1_avx2(parent_values, target_values, w_sub0, w_sub1,
                                       w_add0, count);
            return;
        }
#endif
        if (overflow_safe_) {
            for (std::size_t hidden = 0; hidden < count; ++hidden) {
                target_values[hidden] =
                    parent_values[hidden] - w_sub0[hidden] - w_sub1[hidden] + w_add0[hidden];
            }
            return;
        }
        constexpr std::int32_t kSafeLimit =
            std::numeric_limits<std::int32_t>::max() - 131072;
        for (std::size_t hidden = 0; hidden < count; ++hidden) {
            const std::int32_t p = parent_values[hidden];
            const std::int32_t s0 = w_sub0[hidden];
            const std::int32_t s1 = w_sub1[hidden];
            const std::int32_t a0 = w_add0[hidden];
            if (p > -kSafeLimit && p < kSafeLimit) [[likely]] {
                target_values[hidden] = p - s0 - s1 + a0;
            } else {
                std::int64_t v = static_cast<std::int64_t>(p) - s0;
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v -= s1;
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += a0;
                target_values[hidden] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    v, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
        }
        return;
    }
    if (removed.size() == 2U && added.size() == 2U) {
        const std::int16_t* const w_sub0 =
            base + static_cast<std::size_t>(removed[0]) * count;
        const std::int16_t* const w_sub1 =
            base + static_cast<std::size_t>(removed[1]) * count;
        const std::int16_t* const w_add0 =
            base + static_cast<std::size_t>(added[0]) * count;
        const std::int16_t* const w_add1 =
            base + static_cast<std::size_t>(added[1]) * count;
#if KOI_NNUE_COMPILED_AVX2
        if (avx2_available() && count % 16U == 0U) {
            apply_fused_sub2_add2_avx2(parent_values, target_values, w_sub0, w_sub1,
                                       w_add0, w_add1, count);
            return;
        }
#endif
        if (overflow_safe_) {
            for (std::size_t hidden = 0; hidden < count; ++hidden) {
                target_values[hidden] =
                    parent_values[hidden] - w_sub0[hidden] + w_add0[hidden] -
                    w_sub1[hidden] + w_add1[hidden];
            }
            return;
        }
        constexpr std::int32_t kSafeLimit =
            std::numeric_limits<std::int32_t>::max() - 131072;
        for (std::size_t hidden = 0; hidden < count; ++hidden) {
            const std::int32_t p = parent_values[hidden];
            const std::int32_t s0 = w_sub0[hidden];
            const std::int32_t s1 = w_sub1[hidden];
            const std::int32_t a0 = w_add0[hidden];
            const std::int32_t a1 = w_add1[hidden];
            if (p > -kSafeLimit && p < kSafeLimit) [[likely]] {
                target_values[hidden] = p - s0 + a0 - s1 + a1;
            } else {
                std::int64_t v = static_cast<std::int64_t>(p) - s0;
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += a0;
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v -= s1;
                v = std::clamp<std::int64_t>(v, std::numeric_limits<std::int32_t>::min(),
                                             std::numeric_limits<std::int32_t>::max());
                v += a1;
                target_values[hidden] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    v, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
        }
    }
}

namespace {

#if defined(__clang__) || defined(__GNUC__)
#define KOI_DELTA_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define KOI_DELTA_ALWAYS_INLINE __forceinline
#else
#define KOI_DELTA_ALWAYS_INLINE inline
#endif

KOI_DELTA_ALWAYS_INLINE void apply_fused_s16_sub1_add1(
    const std::int16_t* const base,
    const std::size_t count,
    const std::int16_t* const parent_values,
    std::int16_t* const target_values,
    const std::uint16_t sub0,
    const std::uint16_t add0) noexcept {
    const std::int16_t* const w_sub0 = base + static_cast<std::size_t>(sub0) * count;
    const std::int16_t* const w_add0 = base + static_cast<std::size_t>(add0) * count;
    std::size_t hidden = 0;
#if KOI_NNUE_COMPILED_NEON
    for (; hidden + 16U <= count; hidden += 16U) {
        const int16x8_t p0 = vld1q_s16(parent_values + hidden);
        const int16x8_t p1 = vld1q_s16(parent_values + hidden + 8U);
        const int16x8_t s0_0 = vld1q_s16(w_sub0 + hidden);
        const int16x8_t s0_1 = vld1q_s16(w_sub0 + hidden + 8U);
        const int16x8_t a0_0 = vld1q_s16(w_add0 + hidden);
        const int16x8_t a0_1 = vld1q_s16(w_add0 + hidden + 8U);
        vst1q_s16(target_values + hidden, vaddq_s16(vsubq_s16(p0, s0_0), a0_0));
        vst1q_s16(target_values + hidden + 8U, vaddq_s16(vsubq_s16(p1, s0_1), a0_1));
    }
    for (; hidden + 8U <= count; hidden += 8U) {
        vst1q_s16(target_values + hidden,
                  vaddq_s16(vsubq_s16(vld1q_s16(parent_values + hidden),
                                      vld1q_s16(w_sub0 + hidden)),
                            vld1q_s16(w_add0 + hidden)));
    }
#elif KOI_NNUE_COMPILED_AVX2
    if (avx2_available()) {
        for (; hidden + 16U <= count; hidden += 16U) {
            const __m256i p = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(parent_values + hidden));
            const __m256i s0 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_sub0 + hidden));
            const __m256i a0 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_add0 + hidden));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(target_values + hidden),
                                _mm256_add_epi16(_mm256_sub_epi16(p, s0), a0));
        }
    }
#endif
    for (; hidden < count; ++hidden) {
        target_values[hidden] = static_cast<std::int16_t>(
            parent_values[hidden] - w_sub0[hidden] + w_add0[hidden]);
    }
}

KOI_DELTA_ALWAYS_INLINE void apply_fused_s16_sub2_add1(
    const std::int16_t* const base,
    const std::size_t count,
    const std::int16_t* const parent_values,
    std::int16_t* const target_values,
    const std::uint16_t sub0,
    const std::uint16_t sub1,
    const std::uint16_t add0) noexcept {
    const std::int16_t* const w_sub0 = base + static_cast<std::size_t>(sub0) * count;
    const std::int16_t* const w_sub1 = base + static_cast<std::size_t>(sub1) * count;
    const std::int16_t* const w_add0 = base + static_cast<std::size_t>(add0) * count;
    std::size_t hidden = 0;
#if KOI_NNUE_COMPILED_NEON
    for (; hidden + 16U <= count; hidden += 16U) {
        const int16x8_t p0 = vld1q_s16(parent_values + hidden);
        const int16x8_t p1 = vld1q_s16(parent_values + hidden + 8U);
        const int16x8_t s0_0 = vld1q_s16(w_sub0 + hidden);
        const int16x8_t s0_1 = vld1q_s16(w_sub0 + hidden + 8U);
        const int16x8_t s1_0 = vld1q_s16(w_sub1 + hidden);
        const int16x8_t s1_1 = vld1q_s16(w_sub1 + hidden + 8U);
        const int16x8_t a0_0 = vld1q_s16(w_add0 + hidden);
        const int16x8_t a0_1 = vld1q_s16(w_add0 + hidden + 8U);
        vst1q_s16(target_values + hidden,
                  vaddq_s16(vsubq_s16(vsubq_s16(p0, s0_0), s1_0), a0_0));
        vst1q_s16(target_values + hidden + 8U,
                  vaddq_s16(vsubq_s16(vsubq_s16(p1, s0_1), s1_1), a0_1));
    }
    for (; hidden + 8U <= count; hidden += 8U) {
        vst1q_s16(target_values + hidden,
                  vaddq_s16(vsubq_s16(vsubq_s16(vld1q_s16(parent_values + hidden),
                                                vld1q_s16(w_sub0 + hidden)),
                                      vld1q_s16(w_sub1 + hidden)),
                            vld1q_s16(w_add0 + hidden)));
    }
#elif KOI_NNUE_COMPILED_AVX2
    if (avx2_available()) {
        for (; hidden + 16U <= count; hidden += 16U) {
            const __m256i p = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(parent_values + hidden));
            const __m256i s0 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_sub0 + hidden));
            const __m256i s1 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_sub1 + hidden));
            const __m256i a0 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_add0 + hidden));
            _mm256_storeu_si256(
                reinterpret_cast<__m256i*>(target_values + hidden),
                _mm256_add_epi16(_mm256_sub_epi16(_mm256_sub_epi16(p, s0), s1), a0));
        }
    }
#endif
    for (; hidden < count; ++hidden) {
        target_values[hidden] = static_cast<std::int16_t>(
            parent_values[hidden] - w_sub0[hidden] - w_sub1[hidden] + w_add0[hidden]);
    }
}

KOI_DELTA_ALWAYS_INLINE void apply_fused_s16_sub2_add2(
    const std::int16_t* const base,
    const std::size_t count,
    const std::int16_t* const parent_values,
    std::int16_t* const target_values,
    const std::uint16_t sub0,
    const std::uint16_t sub1,
    const std::uint16_t add0,
    const std::uint16_t add1) noexcept {
    const std::int16_t* const w_sub0 = base + static_cast<std::size_t>(sub0) * count;
    const std::int16_t* const w_sub1 = base + static_cast<std::size_t>(sub1) * count;
    const std::int16_t* const w_add0 = base + static_cast<std::size_t>(add0) * count;
    const std::int16_t* const w_add1 = base + static_cast<std::size_t>(add1) * count;
    std::size_t hidden = 0;
#if KOI_NNUE_COMPILED_NEON
    for (; hidden + 16U <= count; hidden += 16U) {
        const int16x8_t p0 = vld1q_s16(parent_values + hidden);
        const int16x8_t p1 = vld1q_s16(parent_values + hidden + 8U);
        const int16x8_t d0 = vaddq_s16(
            vsubq_s16(vld1q_s16(w_add0 + hidden), vld1q_s16(w_sub0 + hidden)),
            vsubq_s16(vld1q_s16(w_add1 + hidden), vld1q_s16(w_sub1 + hidden)));
        const int16x8_t d1 = vaddq_s16(
            vsubq_s16(vld1q_s16(w_add0 + hidden + 8U), vld1q_s16(w_sub0 + hidden + 8U)),
            vsubq_s16(vld1q_s16(w_add1 + hidden + 8U), vld1q_s16(w_sub1 + hidden + 8U)));
        vst1q_s16(target_values + hidden, vaddq_s16(p0, d0));
        vst1q_s16(target_values + hidden + 8U, vaddq_s16(p1, d1));
    }
    for (; hidden + 8U <= count; hidden += 8U) {
        const int16x8_t d = vaddq_s16(
            vsubq_s16(vld1q_s16(w_add0 + hidden), vld1q_s16(w_sub0 + hidden)),
            vsubq_s16(vld1q_s16(w_add1 + hidden), vld1q_s16(w_sub1 + hidden)));
        vst1q_s16(target_values + hidden,
                  vaddq_s16(vld1q_s16(parent_values + hidden), d));
    }
#elif KOI_NNUE_COMPILED_AVX2
    if (avx2_available()) {
        for (; hidden + 16U <= count; hidden += 16U) {
            const __m256i p = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(parent_values + hidden));
            const __m256i s0 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_sub0 + hidden));
            const __m256i s1 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_sub1 + hidden));
            const __m256i a0 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_add0 + hidden));
            const __m256i a1 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(w_add1 + hidden));
            const __m256i d = _mm256_add_epi16(
                _mm256_sub_epi16(a0, s0), _mm256_sub_epi16(a1, s1));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(target_values + hidden),
                                _mm256_add_epi16(p, d));
        }
    }
#endif
    for (; hidden < count; ++hidden) {
        target_values[hidden] = static_cast<std::int16_t>(
            parent_values[hidden] - w_sub0[hidden] + w_add0[hidden] -
            w_sub1[hidden] + w_add1[hidden]);
    }
}

#undef KOI_DELTA_ALWAYS_INLINE

} // namespace

void NnueWorker::apply_fused_perspective_deltas(
    const std::int16_t* const parent_values,
    std::int16_t* const target_values,
    const std::span<const std::uint16_t> removed,
    const std::span<const std::uint16_t> added) {
    const std::int16_t* const base = weights_->feature_weights.data();
    const std::size_t count = hidden_units_;
    if (removed.size() == 1U && added.size() == 1U) {
        apply_fused_s16_sub1_add1(base, count, parent_values, target_values, removed[0], added[0]);
        return;
    }
    if (removed.size() == 2U && added.size() == 1U) {
        apply_fused_s16_sub2_add1(
            base, count, parent_values, target_values, removed[0], removed[1], added[0]);
        return;
    }
    if (removed.size() == 2U && added.size() == 2U) {
        apply_fused_s16_sub2_add2(
            base, count, parent_values, target_values, removed[0], removed[1], added[0], added[1]);
    }
}

void NnueWorker::store_threat_list(const std::size_t perspective, const std::size_t slot,
                                   const NnueSparseFeaturesThreatV1& list) {
    std::copy_n(list.indices.data(), list.count,
                slot_threats_[perspective].data() + slot * kThreatListCapacity);
    slot_threat_counts_[perspective][slot] = static_cast<std::uint8_t>(list.count);
}

void NnueWorker::apply_threat_deltas(const EvaluationFeatures& child_features,
                                     const Color perspective, const std::size_t slot) {
    const std::size_t index = perspective == Color::white ? 0U : 1U;
    const NnueSparseFeaturesThreatV1 child_list =
        EvaluationFeatureExtractor::encode_sparse_threat_v1(child_features, perspective);
    const std::uint16_t* previous =
        slot_threats_[index].data() + slot * kThreatListCapacity;
    const std::size_t previous_count = slot_threat_counts_[index][slot];
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (old_index < previous_count && new_index < child_list.count) {
        if (previous[old_index] == child_list.indices[new_index]) {
            ++old_index;
            ++new_index;
        } else if (previous[old_index] < child_list.indices[new_index]) {
            apply_feature_delta(index, slot, previous[old_index], -1);
            ++old_index;
        } else {
            apply_feature_delta(index, slot, child_list.indices[new_index], 1);
            ++new_index;
        }
    }
    for (; old_index < previous_count; ++old_index) {
        apply_feature_delta(index, slot, previous[old_index], -1);
    }
    for (; new_index < child_list.count; ++new_index) {
        apply_feature_delta(index, slot, child_list.indices[new_index], 1);
    }
    store_threat_list(index, slot, child_list);
}

void NnueWorker::apply_move_deltas(
    const GameState& child,
    const MoveMetadata& metadata,
    const std::size_t slot,
    const std::array<const std::int32_t*, kIncrementalPerspectives>& parent_values) {
    // The child's feature view is needed when a perspective's own king
    // crosses a bucket boundary and, for v5, to diff the threat feature set.
    // Everything else is derived from the move metadata, which keeps the hot
    // make path allocation- and scan-free for v4.
    const Color mover = opposite(child.side_to_move());
    const Square from = metadata.move.from();
    const Square to = metadata.move.to();
    const MoveKind kind = metadata.kind;
    const PieceType moving = metadata.moving_piece;
    // The slot metadata was initialized from the parent, so its piece count is
    // the parent count.  A refresh stores the exact child count, so the
    // decrement below is skipped in that case.
    const std::uint8_t parent_pieces = slot_pieces_[slot];
    std::array<bool, kIncrementalPerspectives> refreshed{};
    for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
        const Color real = index == 0U ? Color::white : Color::black;
        if (moving == PieceType::king && mover == real) {
            // Only a bucket crossing needs a rebuild; a king step inside one
            // bucket is a plain remove/add pair and castling's rook delta is
            // applied below.  The rebuild covers the king and the castling
            // rook together.
            const std::size_t child_bucket =
                EvaluationFeatureExtractor::halfka_king_bucket_for_square(to, real);
            if (child_bucket != slot_buckets_[index][slot]) {
                if (!threat_features_) {
                    refresh_slot_perspective_v4_finny(child, real, slot);
                } else {
                    refresh_slot_perspective(
                        EvaluationFeatureExtractor::extract(child), real, slot);
                }
                refreshed[index] = true;
                continue;
            }
        }
        const std::size_t bucket = slot_buckets_[index][slot];
        std::array<std::uint16_t, 2> removed{};
        std::array<std::uint16_t, 2> added{};
        std::size_t removed_count = 0;
        std::size_t added_count = 0;
        removed[removed_count++] =
            EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                bucket, real, Piece{moving, mover}, from);
        if (metadata.captured_piece != PieceType::none) {
            Square captured = to;
            if (kind == MoveKind::en_passant) {
                captured = mover == Color::white ?
                    Square::from_index(static_cast<std::uint8_t>(to.index() - 8U)) :
                    Square::from_index(static_cast<std::uint8_t>(to.index() + 8U));
            }
            removed[removed_count++] =
                EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                    bucket, real, Piece{metadata.captured_piece, opposite(mover)},
                    captured);
        }
        const PieceType placed = kind == MoveKind::promotion ?
            promotion_piece_type(metadata.move.promotion()) : moving;
        added[added_count++] =
            EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                bucket, real, Piece{placed, mover}, to);
        if (kind == MoveKind::castling) {
            const bool king_side = mover == Color::white ? to.index() == 6U :
                                                           to.index() == 62U;
            const std::uint8_t rook_from = mover == Color::white ?
                (king_side ? 7U : 0U) : (king_side ? 63U : 56U);
            const std::uint8_t rook_to = mover == Color::white ?
                (king_side ? 5U : 3U) : (king_side ? 61U : 59U);
            removed[removed_count++] =
                EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                    bucket, real, Piece{PieceType::rook, mover},
                    Square::from_index(rook_from));
            added[added_count++] =
                EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                    bucket, real, Piece{PieceType::rook, mover},
                    Square::from_index(rook_to));
        }
        apply_fused_perspective_deltas(
            parent_values[index],
            slot_values_[index].data() + slot * hidden_units_,
            std::span<const std::uint16_t>(removed.data(), removed_count),
            std::span<const std::uint16_t>(added.data(), added_count));
    }
    if (!refreshed[0] && !refreshed[1]) {
        slot_pieces_[slot] = static_cast<std::uint8_t>(
            parent_pieces - (metadata.captured_piece != PieceType::none ? 1U : 0U));
    }
    if (threat_features_) {
        const EvaluationFeatures child_features =
            EvaluationFeatureExtractor::extract(child);
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            if (!refreshed[index]) {
                apply_threat_deltas(child_features,
                    index == 0U ? Color::white : Color::black, slot);
            }
        }
    }
}

bool NnueWorker::prepare_child_slot_metadata(
    const int ply, const int child_slot, const std::uint64_t parent_key,
    std::array<const std::int32_t*, kIncrementalPerspectives>& parent_values) {
    const std::size_t child = static_cast<std::size_t>(child_slot);
    if (scratch_valid_ && scratch_key_ == parent_key) {
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            parent_values[index] = scratch_values_[index].data();
            slot_buckets_[index][child] = scratch_buckets_[index];
            if (threat_features_) {
                slot_threat_counts_[index][child] = scratch_threat_counts_[index];
                std::copy_n(scratch_threats_[index].data(), kThreatListCapacity,
                            slot_threats_[index].data() + child * kThreatListCapacity);
            }
        }
        slot_pieces_[child] = scratch_pieces_;
        return true;
    }
    if (slot_cursor_ == ply && slot_valid_[static_cast<std::size_t>(ply)] != 0 &&
        slot_keys_[static_cast<std::size_t>(ply)] == parent_key) {
        const std::size_t from_slot = static_cast<std::size_t>(ply);
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            parent_values[index] = slot_values_[index].data() + from_slot * hidden_units_;
            slot_buckets_[index][child] = slot_buckets_[index][from_slot];
            if (threat_features_) {
                slot_threat_counts_[index][child] = slot_threat_counts_[index][from_slot];
                std::copy_n(slot_threats_[index].data() + from_slot * kThreatListCapacity,
                            kThreatListCapacity,
                            slot_threats_[index].data() + child * kThreatListCapacity);
            }
        }
        slot_pieces_[child] = slot_pieces_[from_slot];
        return true;
    }
    slot_valid_[child] = 0;
    slot_cursor_ = -1;
    scratch_valid_ = false;
    ++incremental_fallback_count_;
    return false;
}

bool NnueWorker::prepare_child_slot(const int ply, const int child_slot,
                                    const std::uint64_t parent_key) {
    const std::size_t child = static_cast<std::size_t>(child_slot);
    if (scratch_valid_ && scratch_key_ == parent_key) {
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            std::copy_n(scratch_values_[index].data(), hidden_units_,
                        slot_values_[index].data() + child * hidden_units_);
            slot_buckets_[index][child] = scratch_buckets_[index];
            if (threat_features_) {
                slot_threat_counts_[index][child] = scratch_threat_counts_[index];
                std::copy_n(scratch_threats_[index].data(), kThreatListCapacity,
                            slot_threats_[index].data() + child * kThreatListCapacity);
            }
        }
        slot_pieces_[child] = scratch_pieces_;
        return true;
    }
    if (slot_cursor_ == ply && slot_valid_[static_cast<std::size_t>(ply)] != 0 &&
        slot_keys_[static_cast<std::size_t>(ply)] == parent_key) {
        copy_slot_forward(static_cast<std::size_t>(ply), child);
        return true;
    }
    slot_valid_[child] = 0;
    slot_cursor_ = -1;
    scratch_valid_ = false;
    ++incremental_fallback_count_;
    return false;
}

bool NnueWorker::prepare_lazy_parent_slot_v4(const int ply, const std::size_t child,
                                             const std::uint64_t parent_key) {
    const std::size_t from_slot = static_cast<std::size_t>(ply);
    if (scratch_valid_ && scratch_key_ == parent_key) {
        if (slot_valid_[from_slot] == 0 || slot_keys_[from_slot] != parent_key ||
            slot_computed_[0][from_slot] == 0 || slot_computed_[1][from_slot] == 0) {
            for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
                if (int16_safe_) {
                    std::copy_n(scratch_values_16_[index].data(), hidden_units_,
                                slot_values_16_[index].data() + from_slot * hidden_units_);
                } else {
                    std::copy_n(scratch_values_[index].data(), hidden_units_,
                                slot_values_[index].data() + from_slot * hidden_units_);
                }
                slot_buckets_[index][from_slot] = scratch_buckets_[index];
                slot_computed_[index][from_slot] = 1;
            }
            slot_pieces_[from_slot] = scratch_pieces_;
            slot_keys_[from_slot] = parent_key;
            slot_valid_[from_slot] = 1;
        }
        return true;
    }
    if (slot_cursor_ == ply && slot_valid_[from_slot] != 0 &&
        slot_keys_[from_slot] == parent_key) {
        return true;
    }
    slot_valid_[child] = 0;
    slot_cursor_ = -1;
    scratch_valid_ = false;
    ++incremental_fallback_count_;
    return false;
}

template <typename SumT>
void NnueWorker::materialize_slot_perspective_v4_impl(const GameState& state,
                                                      const std::size_t index,
                                                      const std::size_t slot) {
    if (slot_computed_[index][slot] != 0) {
        return;
    }
    auto* values_base = [&]() noexcept -> SumT* {
        if constexpr (std::is_same_v<SumT, std::int16_t>) {
            return slot_values_16_[index].data();
        } else {
            return slot_values_[index].data();
        }
    }();
    std::size_t s = slot;
    while (slot_computed_[index][s] == 0) {
        if (s == 0 || slot_buckets_[index][s] != slot_buckets_[index][s - 1U]) {
            const Color real = index == 0U ? Color::white : Color::black;
            refresh_finny_bucket_v4_impl<SumT>(
                state, real, slot_buckets_[index][slot],
                values_base + slot * hidden_units_);
            slot_computed_[index][slot] = 1;
            return;
        }
        --s;
    }
    const Color real = index == 0U ? Color::white : Color::black;
    const std::int16_t* const weight_base = weights_->feature_weights.data();
    const std::size_t count = hidden_units_;
    const auto feature_idx = [&](const std::size_t cache_slot, const Piece pc,
                                 const Square sq) noexcept -> std::uint16_t {
        return halfka_hm_v9_ ?
            EvaluationFeatureExtractor::halfka_hm_v2_feature_index(cache_slot, real, pc, sq) :
            EvaluationFeatureExtractor::halfka_king_bucket_feature_index(cache_slot, real, pc, sq);
    };
    for (std::size_t cur = s + 1U; cur <= slot; ++cur) {
        const PendingMoveV4& pm = slot_pending_move_[cur];
        const SumT* const parent_ptr = values_base + (cur - 1U) * count;
        SumT* const target_ptr = values_base + cur * count;
        if (pm.is_null) {
            std::copy_n(parent_ptr, count, target_ptr);
        } else {
            const std::size_t bucket = slot_buckets_[index][cur];
            const Square from = pm.move.from();
            const Square to = pm.move.to();
            const std::uint16_t sub0 = feature_idx(bucket, Piece{pm.moving_piece, pm.mover}, from);
            const PieceType placed = pm.kind == MoveKind::promotion ?
                promotion_piece_type(pm.move.promotion()) : pm.moving_piece;
            const std::uint16_t add0 = feature_idx(bucket, Piece{placed, pm.mover}, to);
            if constexpr (std::is_same_v<SumT, std::int16_t>) {
                if (pm.captured_piece != PieceType::none) {
                    Square captured = to;
                    if (pm.kind == MoveKind::en_passant) [[unlikely]] {
                        captured = pm.mover == Color::white ?
                            Square::from_index(static_cast<std::uint8_t>(to.index() - 8U)) :
                            Square::from_index(static_cast<std::uint8_t>(to.index() + 8U));
                    }
                    const std::uint16_t sub1 = feature_idx(
                        bucket, Piece{pm.captured_piece, opposite(pm.mover)}, captured);
                    apply_fused_s16_sub2_add1(
                        weight_base, count, parent_ptr, target_ptr, sub0, sub1, add0);
                } else if (pm.kind == MoveKind::castling) [[unlikely]] {
                    const bool king_side = pm.mover == Color::white ? to.index() == 6U :
                                                                      to.index() == 62U;
                    const std::uint8_t rook_from = pm.mover == Color::white ?
                        (king_side ? 7U : 0U) : (king_side ? 63U : 56U);
                    const std::uint8_t rook_to = pm.mover == Color::white ?
                        (king_side ? 5U : 3U) : (king_side ? 61U : 59U);
                    const std::uint16_t sub1 = feature_idx(
                        bucket, Piece{PieceType::rook, pm.mover}, Square::from_index(rook_from));
                    const std::uint16_t add1 = feature_idx(
                        bucket, Piece{PieceType::rook, pm.mover}, Square::from_index(rook_to));
                    apply_fused_s16_sub2_add2(
                        weight_base, count, parent_ptr, target_ptr, sub0, sub1, add0, add1);
                } else {
                    apply_fused_s16_sub1_add1(
                        weight_base, count, parent_ptr, target_ptr, sub0, add0);
                }
            } else {
                std::array<std::uint16_t, 2> removed{sub0, 0};
                std::array<std::uint16_t, 2> added{add0, 0};
                std::size_t removed_count = 1U;
                std::size_t added_count = 1U;
                if (pm.captured_piece != PieceType::none) {
                    Square captured = to;
                    if (pm.kind == MoveKind::en_passant) {
                        captured = pm.mover == Color::white ?
                            Square::from_index(static_cast<std::uint8_t>(to.index() - 8U)) :
                            Square::from_index(static_cast<std::uint8_t>(to.index() + 8U));
                    }
                    removed[removed_count++] = feature_idx(
                        bucket, Piece{pm.captured_piece, opposite(pm.mover)}, captured);
                }
                if (pm.kind == MoveKind::castling) {
                    const bool king_side = pm.mover == Color::white ? to.index() == 6U :
                                                                      to.index() == 62U;
                    const std::uint8_t rook_from = pm.mover == Color::white ?
                        (king_side ? 7U : 0U) : (king_side ? 63U : 56U);
                    const std::uint8_t rook_to = pm.mover == Color::white ?
                        (king_side ? 5U : 3U) : (king_side ? 61U : 59U);
                    removed[removed_count++] = feature_idx(
                        bucket, Piece{PieceType::rook, pm.mover}, Square::from_index(rook_from));
                    added[added_count++] = feature_idx(
                        bucket, Piece{PieceType::rook, pm.mover}, Square::from_index(rook_to));
                }
                apply_fused_perspective_deltas(
                    parent_ptr, target_ptr,
                    std::span<const std::uint16_t>(removed.data(), removed_count),
                    std::span<const std::uint16_t>(added.data(), added_count));
            }
        }
        slot_computed_[index][cur] = 1;
    }
}

void NnueWorker::materialize_slot_perspective_v4(const GameState& state,
                                                 const std::size_t index,
                                                 const std::size_t slot) {
    materialize_slot_perspective_v4_impl<std::int32_t>(state, index, slot);
}

void NnueWorker::on_make_move(const GameState& child, const MoveMetadata& metadata,
                              const int ply, const std::uint64_t parent_key) {
    if (!supports_incremental()) {
        return;
    }
    if (!storage_ready_) [[unlikely]] {
        ensure_incremental_storage();
    }
    const int child_slot = ply + 1;
    if (ply < 0 || static_cast<std::size_t>(child_slot) >= kIncrementalSlotCount) {
        slot_cursor_ = -1;
        scratch_valid_ = false;
        ++incremental_fallback_count_;
        return;
    }
    const std::size_t child_idx = static_cast<std::size_t>(child_slot);
    if (!threat_features_) {
        const std::size_t from_slot = static_cast<std::size_t>(ply);
        if (!(!scratch_valid_ && slot_cursor_ == ply &&
              slot_valid_[from_slot] != 0 && slot_keys_[from_slot] == parent_key)) {
            if (!prepare_lazy_parent_slot_v4(ply, child_idx, parent_key)) {
                return;
            }
        }
        const Color mover = opposite(child.side_to_move());
        const PieceType moving = metadata.moving_piece;
        if (moving == PieceType::king) {
            const std::size_t mover_idx = mover == Color::white ? 0U : 1U;
            slot_buckets_[mover_idx][child_idx] = static_cast<std::uint8_t>(
                halfka_hm_v9_ ?
                    EvaluationFeatureExtractor::halfka_hm_v2_finny_slot_for_square(
                        metadata.move.to(), mover) :
                    EvaluationFeatureExtractor::halfka_king_bucket_for_square(
                        metadata.move.to(), mover));
            slot_buckets_[1U - mover_idx][child_idx] =
                slot_buckets_[1U - mover_idx][from_slot];
        } else {
            slot_buckets_[0][child_idx] = slot_buckets_[0][from_slot];
            slot_buckets_[1][child_idx] = slot_buckets_[1][from_slot];
        }
        PendingMoveV4& pm = slot_pending_move_[child_idx];
        pm.move = metadata.move;
        pm.moving_piece = moving;
        pm.captured_piece = metadata.captured_piece;
        pm.kind = metadata.kind;
        pm.mover = mover;
        pm.is_null = false;
        slot_computed_[0][child_idx] = 0;
        slot_computed_[1][child_idx] = 0;
        slot_pieces_[child_idx] = static_cast<std::uint8_t>(
            slot_pieces_[from_slot] - (metadata.captured_piece != PieceType::none ? 1U : 0U));
        slot_keys_[child_idx] = child.position_key();
        slot_valid_[child_idx] = 1;
        slot_cursor_ = child_slot;
        return;
    }
    std::array<const std::int32_t*, kIncrementalPerspectives> parent_values{};
    if (!prepare_child_slot_metadata(ply, child_slot, parent_key, parent_values)) {
        return;
    }
    apply_move_deltas(child, metadata, child_idx, parent_values);
    slot_keys_[child_idx] = child.position_key();
    slot_valid_[child_idx] = 1;
    slot_cursor_ = child_slot;
}

void NnueWorker::on_unmake_move(const int child_ply) {
    if (!supports_incremental()) {
        return;
    }
    const int parent = child_ply - 1;
    slot_cursor_ = (parent >= 0 &&
                    static_cast<std::size_t>(parent) < kIncrementalSlotCount &&
                    slot_valid_[static_cast<std::size_t>(parent)] != 0) ? parent : -1;
}

void NnueWorker::on_make_null_move(const GameState& child, const int ply,
                                   const std::uint64_t parent_key) {
    if (!supports_incremental()) {
        return;
    }
    if (!storage_ready_) [[unlikely]] {
        ensure_incremental_storage();
    }
    const int child_slot = ply + 1;
    if (ply < 0 || static_cast<std::size_t>(child_slot) >= kIncrementalSlotCount) {
        slot_cursor_ = -1;
        scratch_valid_ = false;
        ++incremental_fallback_count_;
        return;
    }
    const std::size_t child_idx = static_cast<std::size_t>(child_slot);
    if (!threat_features_) {
        const std::size_t from_slot = static_cast<std::size_t>(ply);
        if (!(!scratch_valid_ && slot_cursor_ == ply &&
              slot_valid_[from_slot] != 0 && slot_keys_[from_slot] == parent_key)) {
            if (!prepare_lazy_parent_slot_v4(ply, child_idx, parent_key)) {
                return;
            }
        }
        slot_buckets_[0][child_idx] = slot_buckets_[0][from_slot];
        slot_buckets_[1][child_idx] = slot_buckets_[1][from_slot];
        slot_computed_[0][child_idx] = 0;
        slot_computed_[1][child_idx] = 0;
        slot_pending_move_[child_idx].is_null = true;
        slot_pieces_[child_idx] = slot_pieces_[from_slot];
        slot_keys_[child_idx] = child.position_key();
        slot_valid_[child_idx] = 1;
        slot_cursor_ = child_slot;
        return;
    }
    if (!prepare_child_slot(ply, child_slot, parent_key)) {
        return;
    }
    slot_keys_[child_idx] = child.position_key();
    slot_valid_[child_idx] = 1;
    slot_cursor_ = child_slot;
}

void NnueWorker::on_unmake_null_move(const int child_ply) {
    on_unmake_move(child_ply);
}

namespace {

class NnueEvaluatorWorker final : public EvaluatorWorker {
public:
    explicit NnueEvaluatorWorker(NnueWorker worker)
        : worker_(std::move(worker)) {}

    [[nodiscard]] int evaluate(const GameState& state, const Color perspective) override {
        return worker_.evaluate_score_only(state, perspective);
    }

    void on_make_move(const GameState& state, const MoveMetadata& metadata, const int ply,
                      const std::uint64_t parent_key) override {
        worker_.on_make_move(state, metadata, ply, parent_key);
    }
    void on_unmake_move(const int child_ply) override {
        worker_.on_unmake_move(child_ply);
    }
    void on_make_null_move(const GameState& state, const int ply,
                           const std::uint64_t parent_key) override {
        worker_.on_make_null_move(state, ply, parent_key);
    }
    void on_unmake_null_move(const int child_ply) override {
        worker_.on_unmake_null_move(child_ply);
    }

private:
    NnueWorker worker_;
};

} // namespace

namespace {

// A network is only usable when its manifest and arrays still validate. Missing
// or rejected network files are a boot/option failure (make_evaluator leaves
// `evaluator` null), while the public evaluator seam guards invalid in-memory
// weights by returning 0 rather than fabricating bogus centipawn scores.
bool network_is_usable(const NnueNetwork& network) {
    return detail::nnue_network_is_usable(network);
}

} // namespace

NnueEvaluator::NnueEvaluator(std::shared_ptr<const NnueNetwork> weights)
    : weights_(std::move(weights)),
      int16_accumulator_safe_(weights_ && network_is_usable(*weights_) ?
          compute_int16_accumulator_safe(*weights_) : false) {}

int NnueEvaluator::evaluate(const GameState& state, const Color perspective) const {
    // The factory only constructs this evaluator for a loaded, usable network;
    // a missing network under default NNUE selection is a boot error, so this
    // is an unreachable defensive path and must not silently become a score.
    if (!weights_ || !network_is_usable(*weights_)) {
        return 0;
    }
    // One-shot evaluation uses the stateless feature overload: the worker is a
    // temporary, so allocating and zeroing the per-ply incremental slots would
    // only add ~1 MB of pointless work to every one-shot evaluation.
    NnueWorker worker(weights_, true, int16_accumulator_safe_);
    return worker.evaluate(EvaluationFeatureExtractor::extract(state), perspective);
}

std::unique_ptr<EvaluatorWorker> NnueEvaluator::create_worker() const {
    if (!weights_ || !network_is_usable(*weights_)) {
        return {};
    }
    return std::make_unique<NnueEvaluatorWorker>(make_worker());
}

NnueWorker NnueEvaluator::make_worker() const {
    return NnueWorker(weights_, true, int16_accumulator_safe_);
}

} // namespace koi
