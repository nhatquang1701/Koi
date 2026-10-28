// Test-only definition of the historical three-argument UciController
// constructor. The engine no longer ships the classical evaluator (HCE removed
// 2026-10-28); this translation unit is built only into the test-support
// library so tests that construct a controller without an explicit evaluator
// keep working during the migration.

#include "koi/uci_controller.hpp"

#include "material_evaluator.hpp"

koi::UciController::UciController(std::istream& input, std::ostream& output,
                                  std::ostream& diagnostics)
    : UciController(input, output, diagnostics,
                    SearchService{std::make_shared<koi::test::MaterialEvaluator>(), {}, 1}) {}
