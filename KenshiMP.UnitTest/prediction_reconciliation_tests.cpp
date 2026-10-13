// KenshiMP.UnitTest - prediction_reconciliation_tests.cpp
// Purpose: Regression coverage for production reconciliation, including wraparound and actor reuse.
#include "prediction_reconciliation_tests.h"
#include "sync/prediction_reconciliation.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

namespace kmp {

namespace {

using TestReporter = void (*)(bool, const char*);
constexpr float FLOAT_TOLERANCE = 0.002f;
constexpr float FRAME_TIME_SEC = 1.f / 60.f;
constexpr int CORRECTION_TEST_FRAMES = 180;

bool FloatEqual(float left, float right) {
    return std::abs(left - right) < FLOAT_TOLERANCE;
}

CharacterPosition MakeSnapshot(EntityID entityId, uint32_t generation, uint32_t sequence) {
    CharacterPosition snapshot{};
    snapshot.entityId = entityId;
    snapshot.generation = generation;
    snapshot.inputSequence = sequence;
    return snapshot;
}

Vec3 DrainCorrection(PredictionReconciliation& reconciliation, EntityID entityId,
                     uint32_t generation) {
    Vec3 total;
    for (int frame = 0; frame < CORRECTION_TEST_FRAMES; ++frame) {
        Vec3 delta;
        if (reconciliation.GetCorrection(entityId, generation, FRAME_TIME_SEC, delta)) {
            total = total + delta;
        }
    }
    return total;
}

void TestPreservesLaterMovement(TestReporter report) {
    PredictionReconciliation reconciliation;
    report(reconciliation.RecordLocalInput(1, 1, 10, Vec3(10.f, 4.f, 2.f), Quat{}),
           "Prediction records the first local sample");
    report(reconciliation.RecordLocalInput(1, 1, 11, Vec3(12.f, 5.f, 6.f), Quat{}),
           "Prediction records later unacknowledged movement");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(1, 1, 10), Vec3(8.f, 4.f, 2.f)) ==
               ReconciliationResult::Accepted,
           "Acknowledgement compares server position with its matching historical sample");

    Vec3 delta;
    report(reconciliation.GetCorrection(1, 1, FRAME_TIME_SEC, delta) &&
               delta.x < 0.f && delta.x > -2.f,
           "Moderate correction blends instead of snapping");
    const Vec3 current = Vec3(12.f, 5.f, 6.f) + delta + DrainCorrection(reconciliation, 1, 1);
    report(FloatEqual(current.x, 10.f) && FloatEqual(current.y, 5.f) && FloatEqual(current.z, 6.f),
           "Correction preserves all movement performed after the acknowledged sample");
    report(!reconciliation.GetCorrection(1, 1, FRAME_TIME_SEC, delta),
           "A fully applied correction cannot be consumed twice");
}

void TestRebasesPendingHistory(TestReporter report) {
    PredictionReconciliation reconciliation;
    reconciliation.RecordLocalInput(2, 1, 1, Vec3(10.f, 0.f, 0.f), Quat{});
    reconciliation.RecordLocalInput(2, 1, 2, Vec3(12.f, 0.f, 0.f), Quat{});
    reconciliation.ReconcileSnapshot(MakeSnapshot(2, 1, 1), Vec3(8.f, 0.f, 0.f));
    Vec3 firstDelta;
    reconciliation.GetCorrection(2, 1, FRAME_TIME_SEC, firstDelta);
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(2, 1, 2), Vec3(10.f, 0.f, 0.f)) ==
               ReconciliationResult::Accepted,
           "A fresh ACK can replace a partially applied correction");
    const Vec3 total = firstDelta + DrainCorrection(reconciliation, 2, 1);
    report(FloatEqual(total.x, -2.f),
           "Rebasing later samples prevents applying an earlier displacement twice");
}

void TestDuplicateAndUnknownAcknowledgements(TestReporter report) {
    PredictionReconciliation reconciliation;
    reconciliation.RecordLocalInput(3, 1, 10, Vec3(1.f, 0.f, 0.f), Quat{});
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(3, 1, 11), Vec3{}) ==
               ReconciliationResult::UnknownInput,
           "Unknown future acknowledgement cannot move the local actor");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(3, 1, 9), Vec3{}) ==
               ReconciliationResult::UnknownInput,
           "Missing historical acknowledgement cannot move the local actor");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(3, 1, 10), Vec3{}) ==
               ReconciliationResult::Accepted,
           "Unknown acknowledgements do not poison the valid acknowledgement cursor");
    const Vec3 firstTotal = DrainCorrection(reconciliation, 3, 1);
    report(FloatEqual(firstTotal.x, -1.f), "Known acknowledgement applies its expected correction");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(3, 1, 10), Vec3(-10.f, 0.f, 0.f)) ==
               ReconciliationResult::DuplicateOrStale,
           "Duplicate ACK cannot schedule a second correction");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(3, 1, 9), Vec3{}) ==
               ReconciliationResult::DuplicateOrStale,
           "Out-of-order ACK cannot rewind the acknowledgement cursor");
    Vec3 delta;
    report(!reconciliation.GetCorrection(3, 1, FRAME_TIME_SEC, delta),
           "Duplicate and stale ACKs leave the actor unchanged");
}

void TestWraparound(TestReporter report) {
    PredictionReconciliation reconciliation;
    const uint32_t lastSequence = std::numeric_limits<uint32_t>::max();
    report(reconciliation.RecordLocalInput(4, 1, lastSequence - 1, Vec3{}, Quat{}) &&
               reconciliation.RecordLocalInput(4, 1, lastSequence, Vec3{}, Quat{}) &&
               reconciliation.RecordLocalInput(4, 1, 0, Vec3{}, Quat{}) &&
               reconciliation.RecordLocalInput(4, 1, 1, Vec3{}, Quat{}),
           "Local sequence history accepts UINT32_MAX to zero wraparound");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(4, 1, lastSequence), Vec3{}) ==
               ReconciliationResult::Accepted &&
               reconciliation.ReconcileSnapshot(MakeSnapshot(4, 1, 0), Vec3{}) ==
               ReconciliationResult::Accepted,
           "Acknowledgement cursor accepts UINT32_MAX to zero wraparound");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(4, 1, lastSequence), Vec3{}) ==
               ReconciliationResult::DuplicateOrStale,
           "Pre-wrap ACK arriving after zero is stale");
    report(!reconciliation.RecordLocalInput(4, 1, 1, Vec3{}, Quat{}) &&
               !reconciliation.RecordLocalInput(4, 1, 0, Vec3{}, Quat{}) &&
               !reconciliation.RecordLocalInput(4, 1, 0x80000001u, Vec3{}, Quat{}),
           "Local duplicate, stale, and ambiguous half-range sequences are rejected");
}

void TestHistoryBoundAndCleanup(TestReporter report) {
    PredictionReconciliation reconciliation;
    for (uint32_t sequence = 1; sequence <= 129; ++sequence) {
        reconciliation.RecordLocalInput(5, 1, sequence, Vec3{}, Quat{});
    }
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(5, 1, 1), Vec3{}) ==
               ReconciliationResult::UnknownInput,
           "The 129th sample evicts the oldest entry from the 128-sample history");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(5, 1, 2), Vec3(1.f, 0.f, 0.f)) ==
               ReconciliationResult::Accepted,
           "Oldest retained sample remains usable after history eviction");
    report(reconciliation.RecordLocalInput(5, 2, 1, Vec3{}, Quat{}),
           "A new actor generation starts with independent sequence history");
    Vec3 delta;
    report(!reconciliation.GetCorrection(5, 1, FRAME_TIME_SEC, delta) &&
               !reconciliation.GetCorrection(5, 2, FRAME_TIME_SEC, delta),
           "Generation change discards the previous actor's queued correction");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(5, 1, 129), Vec3{}) ==
               ReconciliationResult::UnknownInput,
           "Delayed snapshot from a replaced actor cannot clear the new actor history");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(5, 2, 1), Vec3(1.f, 0.f, 0.f)) ==
               ReconciliationResult::Accepted,
           "The current actor generation can still reconcile after a stale snapshot");
    reconciliation.RemoveEntity(5);
    report(!reconciliation.GetCorrection(5, 2, FRAME_TIME_SEC, delta),
           "Entity destruction removes pending corrections");
    reconciliation.RecordLocalInput(5, 3, 1, Vec3{}, Quat{});
    reconciliation.Clear();
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(5, 3, 1), Vec3{}) ==
               ReconciliationResult::UnknownInput,
           "Disconnect or world reset clears all prediction history");
}

void TestDeadzoneSnapAndRestore(TestReporter report) {
    PredictionReconciliation reconciliation;
    reconciliation.RecordLocalInput(6, 1, 1, Vec3{}, Quat{});
    reconciliation.ReconcileSnapshot(MakeSnapshot(6, 1, 1), Vec3(0.01f, 0.f, 0.f));
    Vec3 delta;
    report(!reconciliation.GetCorrection(6, 1, FRAME_TIME_SEC, delta),
           "Sub-deadzone position noise does not jitter the local actor");
    reconciliation.RecordLocalInput(6, 1, 2, Vec3{}, Quat{});
    reconciliation.RecordLocalInput(6, 1, 3, Vec3(2.f, 0.f, 0.f), Quat{});
    reconciliation.ReconcileSnapshot(MakeSnapshot(6, 1, 2), Vec3(100.f, 0.f, 0.f));
    report(reconciliation.GetCorrection(6, 1, FRAME_TIME_SEC, delta) && FloatEqual(delta.x, 100.f),
           "Large correction snaps once without discarding later movement");
    report(!reconciliation.GetCorrection(6, 1, FRAME_TIME_SEC, delta),
           "Large snap is consumed exactly once");
    report(reconciliation.RestoreCorrection(6, 1, Vec3(100.f, 0.f, 0.f)),
           "Failed game-memory write can return a consumed correction");
    report(reconciliation.GetCorrection(6, 1, FRAME_TIME_SEC, delta) && FloatEqual(delta.x, 100.f),
           "Restored correction can be retried on the next game frame");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(6, 1, 3), Vec3(102.f, 0.f, 0.f)) ==
               ReconciliationResult::Accepted &&
               !reconciliation.GetCorrection(6, 1, FRAME_TIME_SEC, delta),
           "Restoring and retrying preserve the coordinate frame of pending samples");
}

void TestFiniteValidationAndFrameCap(TestReporter report) {
    PredictionReconciliation reconciliation;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    report(!reconciliation.RecordLocalInput(INVALID_ENTITY, 1, 1, Vec3{}, Quat{}) &&
               !reconciliation.RecordLocalInput(7, 1, 1, Vec3(nan, 0.f, 0.f), Quat{}) &&
               !reconciliation.RecordLocalInput(7, 1, 1, Vec3{}, Quat(infinity, 0.f, 0.f, 0.f)),
           "Invalid entity IDs and non-finite prediction samples are rejected");
    reconciliation.RecordLocalInput(7, 1, 1, Vec3{}, Quat{});
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(7, 1, 1), Vec3(infinity, 0.f, 0.f)) ==
               ReconciliationResult::Invalid,
           "Non-finite server position cannot poison pending correction");
    reconciliation.ReconcileSnapshot(MakeSnapshot(7, 1, 1), Vec3(2.f, 0.f, 0.f));
    Vec3 delta;
    report(!reconciliation.GetCorrection(7, 1, nan, delta) &&
               !reconciliation.GetCorrection(7, 1, infinity, delta) &&
               !reconciliation.GetCorrection(7, 1, -1.f, delta) &&
               !reconciliation.GetCorrection(7, 1, 0.f, delta),
           "Invalid or paused frame time leaves pending correction unconsumed");
    report(reconciliation.GetCorrection(7, 1, 10.f, delta) && delta.x > 0.f && delta.x < 1.1f,
           "A long frame cannot consume more than the bounded smoothing step");
    report(FloatEqual((delta + DrainCorrection(reconciliation, 7, 1)).x, 2.f),
           "Frame-time validation and capping retain the complete displacement");

    const float largest = std::numeric_limits<float>::max();
    reconciliation.RecordLocalInput(8, 1, 1, Vec3(-largest, 0.f, 0.f), Quat{});
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(8, 1, 1), Vec3(largest, 0.f, 0.f)) ==
               ReconciliationResult::Invalid,
           "Overflowed subtraction of finite positions is rejected");
    report(reconciliation.ReconcileSnapshot(MakeSnapshot(8, 1, 1), Vec3(-largest, 0.f, 0.f)) ==
               ReconciliationResult::Accepted,
           "Rejected overflow does not consume a valid historical acknowledgement");
}

void TestRestoreAcrossAcknowledgement(TestReporter report) {
    PredictionReconciliation reconciliation;
    reconciliation.RecordLocalInput(11, 1, 1, Vec3(10.f, 0.f, 0.f), Quat{});
    reconciliation.RecordLocalInput(11, 1, 2, Vec3(12.f, 0.f, 0.f), Quat{});
    reconciliation.ReconcileSnapshot(MakeSnapshot(11, 1, 1), Vec3(8.f, 0.f, 0.f));
    Vec3 failedDelta;
    reconciliation.GetCorrection(11, 1, FRAME_TIME_SEC, failedDelta);
    reconciliation.ReconcileSnapshot(MakeSnapshot(11, 1, 2), Vec3(10.f, 0.f, 0.f));
    report(reconciliation.RestoreCorrection(11, 1, failedDelta),
           "Failed position write can restore its delta after a concurrent fresh ACK");
    report(FloatEqual(DrainCorrection(reconciliation, 11, 1).x, -2.f),
           "Restoring after a fresh ACK retains exactly the unapplied displacement");
}

void TestConcurrentAccess(TestReporter report) {
    PredictionReconciliation reconciliation;
    std::atomic<bool> failed{false};
    auto worker = [&reconciliation, &failed](EntityID entityId) {
        for (uint32_t sequence = 1; sequence <= 1000; ++sequence) {
            if (!reconciliation.RecordLocalInput(entityId, 1, sequence, Vec3{}, Quat{}) ||
                reconciliation.ReconcileSnapshot(MakeSnapshot(entityId, 1, sequence), Vec3{}) !=
                    ReconciliationResult::Accepted) {
                failed.store(true);
            }
            Vec3 delta;
            reconciliation.GetCorrection(entityId, 1, FRAME_TIME_SEC, delta);
        }
    };
    std::thread first(worker, 9);
    std::thread second(worker, 10);
    first.join();
    second.join();
    report(!failed.load(), "Concurrent record, ACK, and correction access preserves entity state");
}

} // namespace

void RunPredictionReconciliationTests(void (*report)(bool, const char*)) {
    if (report == nullptr) {
        return;
    }
    TestPreservesLaterMovement(report);
    TestRebasesPendingHistory(report);
    TestDuplicateAndUnknownAcknowledgements(report);
    TestWraparound(report);
    TestHistoryBoundAndCleanup(report);
    TestDeadzoneSnapAndRestore(report);
    TestFiniteValidationAndFrameCap(report);
    TestRestoreAcrossAcknowledgement(report);
    TestConcurrentAccess(report);
}

} // namespace kmp
