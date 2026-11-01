#pragma once

// Shared merge for per-worker search statistics.  Worker pools and the serial
// driver both accumulate partial counters into one SearchStats value.

#include "koi/search_types.hpp"

namespace koi::detail {

void accumulate_stats(SearchStats& total, const SearchStats& partial) noexcept {
    total.nodes += partial.nodes;
    total.qnodes += partial.qnodes;
    total.qsearch_cache_hits += partial.qsearch_cache_hits;
    total.qsearch_tt_cutoffs += partial.qsearch_tt_cutoffs;
    total.qsearch_move_generations += partial.qsearch_move_generations;
    total.qsearch_stand_pat_cutoffs += partial.qsearch_stand_pat_cutoffs;
    total.has_legal_move_calls += partial.has_legal_move_calls;
    total.position_feature_extractions += partial.position_feature_extractions;
    total.lmr_parent_feature_reuses += partial.lmr_parent_feature_reuses;
    total.evaluation_cache_hits += partial.evaluation_cache_hits;
    total.evaluation_cache_misses += partial.evaluation_cache_misses;
    total.evaluation_feature_cache_misses += partial.evaluation_feature_cache_misses;
    total.correction_history_updates += partial.correction_history_updates;
    total.tt_hits += partial.tt_hits;
    total.pvs_searches += partial.pvs_searches;
    total.pvs_researches += partial.pvs_researches;
    total.root_pvs_searches += partial.root_pvs_searches;
    total.root_pvs_researches += partial.root_pvs_researches;
    total.root_selective_candidates += partial.root_selective_candidates;
    total.root_selective_researches += partial.root_selective_researches;
    total.short_fallback_invocations += partial.short_fallback_invocations;
    total.short_fallback_candidates += partial.short_fallback_candidates;
    total.short_fallback_overdue_candidates += partial.short_fallback_overdue_candidates;
    total.quiet_forcing_extensions += partial.quiet_forcing_extensions;
    total.aspiration_researches += partial.aspiration_researches;
    total.check_extensions += partial.check_extensions;
    total.king_safety_extensions += partial.king_safety_extensions;
    total.qchecks += partial.qchecks;
    total.see_prunes += partial.see_prunes;
    total.delta_prunes += partial.delta_prunes;
    total.null_cutoffs += partial.null_cutoffs;
    total.null_verifications += partial.null_verifications;
    total.null_repetition_skips += partial.null_repetition_skips;
    total.lmr_reductions += partial.lmr_reductions;
    total.lmr_verifications += partial.lmr_verifications;
    total.lmr_king_zone_exclusions += partial.lmr_king_zone_exclusions;
    total.lmr_high_history_exclusions += partial.lmr_high_history_exclusions;
    total.continuation_history_prunes += partial.continuation_history_prunes;
    total.quiet_futility_prunes += partial.quiet_futility_prunes;
    total.reverse_futility_prunes += partial.reverse_futility_prunes;
    total.razoring_prunes += partial.razoring_prunes;
    total.internal_iterative_deepening += partial.internal_iterative_deepening;
    total.probcut_searches += partial.probcut_searches;
    total.probcut_cutoffs += partial.probcut_cutoffs;
    total.singular_searches += partial.singular_searches;
    total.singular_extensions += partial.singular_extensions;
    total.multi_cut_prunes += partial.multi_cut_prunes;
    total.capture_history_updates += partial.capture_history_updates;
    total.quiet_history_updates += partial.quiet_history_updates;
    total.continuation_history_updates += partial.continuation_history_updates;
    total.tbhits += partial.tbhits;
    total.seldepth = std::max(total.seldepth, partial.seldepth);
}

} // namespace koi::detail
