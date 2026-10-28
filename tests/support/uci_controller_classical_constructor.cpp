// Test-only definition of the historical three-argument UciController
// constructor. The engine no longer ships the classical evaluator (HCE removed
// 2026-10-28); this translation unit is built ONLY into
// `koi_hce_test_support` so tests that construct a controller without an
// explicit evaluator keep working during the migration.

#include "koi/classical_evaluator.hpp"
#include "koi/uci_controller.hpp"

koi::UciController::UciController(std::istream& input, std::ostream& output,
                                  std::ostream& diagnostics)
    : UciController(input, output, diagnostics,
                    SearchService{std::make_shared<ClassicalEvaluator>(), {}, 1}) {}
