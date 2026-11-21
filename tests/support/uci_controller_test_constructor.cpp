// Test-only factory for a UciController backed by the in-repo material
// evaluator.  The production engine requires an NNUE network or explicit
// ClassicalEvaluator injection, so tests use this factory to keep lightweight
// unit fixtures decoupled from NNUE loading.

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
