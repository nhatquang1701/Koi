#pragma once

// Test-only helpers for constructing a UciController with the in-repo material
// evaluator.  Kept out of the shipped controller header, which only exposes the
// production constructor.

#include <iosfwd>
#include <memory>

namespace koi {

class UciController;

namespace test {

[[nodiscard]] std::unique_ptr<UciController> make_uci_controller(
    std::istream& input, std::ostream& output, std::ostream& diagnostics);

} // namespace test
} // namespace koi
