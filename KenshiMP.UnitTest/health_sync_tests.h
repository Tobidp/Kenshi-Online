// KenshiMP.UnitTest - health_sync_tests.h
#pragma once

namespace kmp {
/// Runs health replication regressions using the existing assertion callback.
void RunHealthSyncTests(void (*assertion)(bool, const char*));
} // namespace kmp
