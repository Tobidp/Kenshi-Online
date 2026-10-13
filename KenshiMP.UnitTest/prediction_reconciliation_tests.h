// KenshiMP.UnitTest - prediction_reconciliation_tests.h
// Purpose: Run the production position reconciliation regression tests through the test reporter.
#pragma once

namespace kmp {

void RunPredictionReconciliationTests(void (*report)(bool, const char*));

} // namespace kmp
