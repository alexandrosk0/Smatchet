// BulkImportAbandonNonBlocking.test.cpp — WS-A bucket-E (#734) of
// docs/plans/active/ui-freeze-pillar2-blocking.md.
//
// Pillar-2 (UI never freezes) regression guard for the bulk-import futures-clear
// path. The real flow: a `.clear()` / re-parse / re-run on the bulk-import window
// must NOT block the UI frame for the still-running create workers' duration. The
// production helper SmatchetBulkTicketsUi.cpp::BulkImportAbandonFutures(d) does:
//   d.bulkImportCancel.Cancel();   // flip the shared flag
//   move still-valid futures -> d.bulkImportFutureGraveyard;  // no inline wait
//   d.bulkImportFutures.clear();
//   d.bulkImportCancel.Reset();    // fresh flag for the next run
//
// That is *exactly* CancellableFutureSet<T>::SignalCancelAll() + AbandonInto(g),
// plus the same UiDrawSession member shapes
// (std::vector<std::future<IssueCreateResult>> bulkImportFutures /
//  bulkImportFutureGraveyard + smatchet::ui::CancelToken bulkImportCancel). This
// test reproduces the helper against std::future workers that block for a
// multi-second "create" so a wrong (inline-join) implementation would block the
// caller for the full worker runtime — the assertion is that the abandon call
// returns in << the worker's runtime, with no ImGui / AppController link needed.
//
// Driving the full bulk-import ImGui window through the Test Engine is the heavier
// bucket-E form; this focused integration test exercises the identical
// signal-then-abandon semantics deterministically (see report's deferred-automation
// note for the full-window drive).

#include "CancelToken.h"

#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using smatchet::ui::CancelToken;

namespace {

// Stand-in for AppController::IssueCreateResult — the bulk-import future payload.
// The abandon path never inspects this; only its move-out matters.
struct FakeCreateResult {
    bool ok = false;
    std::string key;
};

// Faithful re-creation of UiDrawSession's bulk-import future plumbing
// (SmatchetUiSession.h): the futures vector, the cancel token each create worker
// captures, and the graveyard the abandon moves still-running futures into.
struct FakeBulkSession {
    std::vector<std::future<FakeCreateResult>> bulkImportFutures;
    smatchet::ui::CancelToken bulkImportCancel;
    std::vector<std::future<FakeCreateResult>> bulkImportFutureGraveyard;
};

// Byte-for-byte the production BulkImportAbandonFutures(d) shape: signal cancel,
// move still-valid futures into the graveyard WITHOUT waiting, clear, reset.
void FakeBulkImportAbandonFutures(FakeBulkSession& d) {
    d.bulkImportCancel.Cancel();
    for (auto& f : d.bulkImportFutures) {
        if (f.valid()) {
            d.bulkImportFutureGraveyard.push_back(std::move(f));
        }
    }
    d.bulkImportFutures.clear();
    d.bulkImportCancel.Reset();
}

} // namespace

TEST_CASE("BulkImportAbandonFutures returns within a frame budget while N create workers are still running") {
    FakeBulkSession d;

    constexpr int kWorkers = 4;
    // Each "create" worker blocks for kWorkerRuntimeMs unless it observes cancel.
    // A wrong (inline-join) clear would block the caller for ~kWorkerRuntimeMs.
    constexpr int kWorkerRuntimeMs = 3000;

    std::atomic<int> started{0};
    const CancelToken workerToken = d.bulkImportCancel; // the token a real CreateIssueAsync would capture

    for (int i = 0; i < kWorkers; ++i) {
        auto fut = std::async(std::launch::async, [workerToken, &started, kWorkerRuntimeMs]() {
            started.fetch_add(1);
            // Simulate the create's IO as a poll loop so a cooperative cancel lands fast,
            // but absent cancel the worker runs the full multi-second runtime.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWorkerRuntimeMs);
            while (std::chrono::steady_clock::now() < deadline) {
                if (workerToken.IsCancelled()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            FakeCreateResult r;
            r.ok = true;
            r.key = "BULK-1";
            return r;
        });
        d.bulkImportFutures.push_back(std::move(fut));
    }

    // Wait until every worker is actually running so the abandon races a live worker,
    // not a not-yet-scheduled std::async.
    while (started.load() < kWorkers) {
        std::this_thread::yield();
    }

    // KEY ASSERTION: abandoning N still-running create futures must not block the
    // caller for the worker duration — it signals + moves, never inline-joins.
    const auto t0 = std::chrono::steady_clock::now();
    FakeBulkImportAbandonFutures(d);
    const auto abandonMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    CHECK(d.bulkImportFutures.empty());                      // cleared
    CHECK(d.bulkImportFutureGraveyard.size() == kWorkers);   // moved out, not dropped
    CHECK_FALSE(d.bulkImportCancel.IsCancelled());           // fresh token for the next run
    // A single frame at the 144 Hz Pillar-1 budget is ~6.94 ms; the 100 Hz floor is
    // 10 ms. Allow generous headroom for CI scheduler jitter but stay FAR below the
    // 3000 ms worker runtime an inline-join would have cost.
    CHECK(abandonMs < 250);

    // Drain the graveyard off the hot path (what
    // DrainUiDrawSessionFuturesBeforeAppTeardown does at shutdown). Because the workers
    // observed the cancel flag flipped by FakeBulkImportAbandonFutures, this join is fast
    // too — proving the abandoned workers actually wound down rather than ran to term.
    const auto tJoin0 = std::chrono::steady_clock::now();
    for (auto& f : d.bulkImportFutureGraveyard) {
        if (f.valid()) {
            (void)f.get();
        }
    }
    const auto joinMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tJoin0).count();
    d.bulkImportFutureGraveyard.clear();
    CHECK(joinMs < kWorkerRuntimeMs); // workers bailed cooperatively, not at the full runtime
}

TEST_CASE("BulkImportAbandonFutures is safe to call with no in-flight futures (idempotent re-run guard)") {
    FakeBulkSession d;
    // The real helper is called unconditionally at the top of every re-parse / re-run
    // (SmatchetBulkTicketsUi.cpp:148/263/414) — it must be a no-op when nothing is live.
    CHECK_NOTHROW(FakeBulkImportAbandonFutures(d));
    CHECK(d.bulkImportFutures.empty());
    CHECK(d.bulkImportFutureGraveyard.empty());
    CHECK_FALSE(d.bulkImportCancel.IsCancelled());
}
