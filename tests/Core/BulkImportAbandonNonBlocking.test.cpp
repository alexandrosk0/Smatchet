// BulkImportAbandonNonBlocking.test.cpp — WS-A bucket-E (#734) of
// docs/plans/ui-freeze-pillar2-blocking.md.
//
// Pillar-2 (UI never freezes) regression guard for the bulk-import futures-clear
// path. The real flow: a `.clear()` / re-parse / re-run on the bulk-import window
// must NOT block the UI frame for the still-running create workers' duration.
//
// This test calls the PRODUCTION helper: smatchet::ui::BulkImportAbandonFutures
// (Source/Core/include/Ui/BulkImportAbandon.h), which is exactly what the three
// futures-clear sites in SmatchetBulkTicketsUi.cpp invoke. No re-statement of the
// signal / move / clear / reset sequence lives here any more (DR29) — mutate the
// helper and these cases go red.
//
// The only stand-in left is the session object. Production's `UiDrawSession`
// (SmatchetUiSession.h) drags ImGui + AppController into any TU that names it,
// which this rig cannot link, so the helper is a template over the session type
// and this test hands it a struct carrying the same three members. The workers
// are real std::futures that block for a multi-second "create", so a wrong
// (inline-join) helper would block the caller for the full worker runtime — the
// assertion is that the abandon call returns in << that runtime.
//
// Driving the full bulk-import ImGui window through the Test Engine is the heavier
// bucket-E form; this focused integration test exercises the identical
// signal-then-abandon semantics deterministically (see report's deferred-automation
// note for the full-window drive).

#include "CancelToken.h"
#include "Ui/BulkImportAbandon.h"

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

// The three UiDrawSession members the production helper touches
// (SmatchetUiSession.h): the futures vector, the cancel token each create worker
// captures, and the graveyard the abandon moves still-running futures into. This
// is data only — the behaviour under test comes from the production template.
struct StubBulkSession {
    std::vector<std::future<FakeCreateResult>> bulkImportFutures;
    smatchet::ui::CancelToken bulkImportCancel;
    std::vector<std::future<FakeCreateResult>> bulkImportFutureGraveyard;
};

} // namespace

TEST_CASE("BulkImportAbandonFutures returns within a frame budget while N create workers are still running") {
    StubBulkSession d;

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
    smatchet::ui::BulkImportAbandonFutures(d);
    const auto abandonMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    CHECK(d.bulkImportFutures.empty());                      // cleared
    CHECK(d.bulkImportFutureGraveyard.size() == kWorkers);   // moved out, not dropped
    CHECK_FALSE(d.bulkImportCancel.IsCancelled());           // fresh token for the next run
    // A single frame at the 144 Hz Pillar-1 budget is ~6.94 ms; the 100 Hz floor is
    // 10 ms. Allow generous headroom for CI scheduler jitter but stay FAR below the
    // 3000 ms worker runtime an inline-join would have cost.
#if defined(__SANITIZE_ADDRESS__)
    CHECK(abandonMs < 2500);  // ASAN ~3-10x wall-clock overhead; budget loosened (#1215 pattern)
#else
    CHECK(abandonMs < 250);
#endif

    // Drain the graveyard off the hot path (what
    // DrainUiDrawSessionFuturesBeforeAppTeardown does at shutdown). Because the workers
    // observed the cancel flag flipped by BulkImportAbandonFutures, this join is fast
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
#if defined(__SANITIZE_ADDRESS__)
    WARN(joinMs < kWorkerRuntimeMs); // cooperative-bail timing isn't ASAN-stable; a 10x loosen would mask a full-runtime inline-join regression
#else
    CHECK(joinMs < kWorkerRuntimeMs); // workers bailed cooperatively, not at the full runtime
#endif
}

TEST_CASE("BulkImportAbandonFutures is safe to call with no in-flight futures (idempotent re-run guard)") {
    StubBulkSession d;
    // The real helper is called unconditionally at the top of every re-parse / re-run
    // (SmatchetBulkTicketsUi.cpp:148/263/414) — it must be a no-op when nothing is live.
    CHECK_NOTHROW(smatchet::ui::BulkImportAbandonFutures(d));
    CHECK(d.bulkImportFutures.empty());
    CHECK(d.bulkImportFutureGraveyard.empty());
    CHECK_FALSE(d.bulkImportCancel.IsCancelled());
}
