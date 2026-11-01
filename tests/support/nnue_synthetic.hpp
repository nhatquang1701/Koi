#pragma once

// Test-only synthetic NNUE fixtures (declared outside the shipped engine API).

#include "koi/nnue.hpp"

namespace koi::test {

[[nodiscard]] NnueNetwork synthetic_network();
[[nodiscard]] NnueNetwork synthetic_network_v2();
[[nodiscard]] NnueNetwork synthetic_network_v4();
[[nodiscard]] NnueNetwork synthetic_network_v5();
[[nodiscard]] NnueNetwork synthetic_network_v6();

} // namespace koi::test
