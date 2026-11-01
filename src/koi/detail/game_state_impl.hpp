#pragma once

// Private implementation of GameState.  Kept in a detail header so the
// consistency-diagnostic unit can build a snapshot from the same
// CompatibilityMirror/Position/FeatureState state without exposing the impl in
// the public GameState header.

#include "koi/game_state.hpp"

#include "koi/detail/compatibility_mirror.hpp"
#include "koi/detail/feature_state.hpp"
#include "koi/detail/native_check_probe.hpp"
#include "koi/position.hpp"

namespace koi {

class GameState::Impl {
public:
    // NativePosition is the legality and incremental-key authority. The
    // vendored board is owned by CompatibilityMirror and remains a private
    // compatibility adapter for Polyglot and diagnostic consumers.
    detail::CompatibilityMirror compatibility_mirror{};
    Position native_position{};
    mutable detail::FeatureState feature_state{};
    // Search-only states disable shadow maintenance (see detach_mirror()).
    // The flag is copied so every search descendant inherits it.
    bool mirror_tracking = true;
    // Cached invariant inputs of the last native_check_probe() built for
    // native_position.  move_gives_check() compares fresh inputs on every call,
    // so a stale cache (for example after a copy, which starts with the default
    // probe) can never change a result.  No mutable is needed: unique_ptr
    // indirection permits mutation from const member functions.
    detail::NativeCheckProbe check_probe{};

    Impl() = default;

    // The feature cache is a pure accelerator: a copied state recomputes the
    // features it needs.  Dropping the 256-slot copy keeps per-node search
    // copies (root-parallel workers, legality probes) inexpensive while
    // preserving identical evaluation results.  The compatibility mirror is
    // copied only while it is tracked: a detached search state never consults
    // it, so descendants must not pay for a second board and its history.
    // The assignment is conditional because mirror_tracking is declared after
    // the mirror and cannot be read from the initializer list.
    Impl(const Impl& other) : native_position(other.native_position),
                              feature_state(),
                              mirror_tracking(other.mirror_tracking) {
        if (mirror_tracking) {
            compatibility_mirror = other.compatibility_mirror;
        }
    }
};

} // namespace koi
