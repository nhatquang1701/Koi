// Test-only factory for a UciController backed by the in-repo material
// evaluator.  The engine no longer ships a classical evaluator (HCE removed
// 2026-10-28), so the historical three-argument constructor cannot live in the
// production class; tests go through this factory instead.

#include "uci_controller_test_support.hpp"

#include <istream>
#include <memory>
#include <ostream>

#include "koi/uci_controller.hpp"

#include "material_evaluator.hpp"

namespace koi::test {

std::unique_ptr<UciController> make_uci_controller(std::istream& input, std::ostream& output,
                                                    std::ostream& diagnostics) {
    return std::make_unique<UciController>(
        input, output, diagnostics,
        SearchService{std::make_shared<MaterialEvaluator>(), {}, 1});
}

} // namespace koi::test
