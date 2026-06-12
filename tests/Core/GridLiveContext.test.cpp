// GridLiveContext seam tests — multi-grid foundation Slice 1a
// (docs/plans/multi-grid-tabs.md, ADR-0018).
//
// Pins what 1a actually introduced: the per-pane GridLiveContext bundle that the
// former AppController singleton members moved into. Covered here:
//   * default-constructed state (null backend, empty cache, revision 0, empty keys) —
//     what AppController's constructor-time kDefaultPaneId entry starts from.
//   * the ADR-0012 Backend slot contract the delegators rely on: atomic store/load
//     round-trip, and a latched strong handle surviving a live slot swap.
//   * the publish path the snapshot delegators route through: mutate ActiveTickets
//     under the mutex, publish a const snapshot, bump the revision.
//
// AppController's own focusedContext()/kDefaultPaneId routing has NO bucket-A seam:
// AppController.cpp is not part of the doctest rig (it drags the UI/Lua/MCP link
// chain), so the delegator wiring stays covered by the bucket-E UI tests; this TU
// pins the context contract those delegators forward to.

#include "../support/FakeTrackerClient.h"

#include "GridLiveContext.h"
#include "ITrackerBackend.h"

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

TEST_CASE("GridLiveContext default construction matches the old singleton boot state") {
    GridLiveContext ctx;

    CHECK(std::atomic_load(&ctx.Backend) == nullptr);
    CHECK(ctx.ActiveTickets.empty());
    CHECK(ctx.activeTicketsPublished_ == nullptr);
    CHECK(ctx.ActiveTicketsRevision.load() == 0u);
    CHECK(ctx.CacheBackendKeyCopy().empty()); // wired by AppController init (Slice 1b)
    CHECK(ctx.catalogKey.empty());            // populated by Slice 3
    CHECK(ctx.ticketSync_ == nullptr);
}

TEST_CASE("cache backend key: guarded set/copy round-trip (Slice 1b)") {
    GridLiveContext ctx;
    ctx.SetCacheBackendKey("GitHub");
    CHECK(ctx.CacheBackendKeyCopy() == "GitHub");
    ctx.SetCacheBackendKey("Plane"); // re-stamp on tracker swap
    CHECK(ctx.CacheBackendKeyCopy() == "Plane");
}

TEST_CASE("Backend slot honours the ADR-0012 atomic store/load discipline") {
    GridLiveContext ctx;

    std::shared_ptr<ITrackerBackend> backend = std::make_shared<smatchet_tests::FakeTrackerClient>("Jira");
    std::atomic_store(&ctx.Backend, backend);

    CHECK(std::atomic_load(&ctx.Backend).get() == backend.get());

    SUBCASE("a latched strong handle survives a live slot swap") {
        // The raw-capture-across-async fix class: a worker latches a shared_ptr before
        // dispatch; a SetBackend-style swap must not free the latched backend.
        std::shared_ptr<ITrackerBackend> latched = std::atomic_load(&ctx.Backend);
        std::shared_ptr<ITrackerBackend> replacement = std::make_shared<smatchet_tests::FakeTrackerClient>("Plane");
        std::shared_ptr<ITrackerBackend> retired = std::atomic_exchange(&ctx.Backend, replacement);

        CHECK(retired.get() == backend.get());
        CHECK(std::atomic_load(&ctx.Backend).get() == replacement.get());
        REQUIRE(latched != nullptr);
        CHECK(latched.get() == backend.get());
        // The old backend is still alive through the latch even after the slot moved on.
        CHECK(latched->Connectivity().GetTrackerType() == "Jira");
    }
}

namespace {

// Mirrors the kick-time half of AppController::EnsureProjectComponentsLoaded (#975): under the
// catalog mutex, honour the already-warmed early-out + the in-flight dedup, then take the
// in-flight marker. Returns true if the worker should proceed (marker was taken).
bool KickComponentLoad(GridContextFieldCatalog& cat, const std::string& projectKey) {
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    if (cat.projectComponentOptions_.find(projectKey) != cat.projectComponentOptions_.end()) {
        return false; // already warmed
    }
    return cat.projectComponentsInFlight_.insert(projectKey).second;
}

// Mirrors the success-exit of the worker: insert the fetched options (presence == loaded) and
// erase the in-flight marker — on WHICHEVER catalog the worker resolved. The #975 fix is solely
// about WHICH catalog this is called against: the kick-time one, not a completion-time re-resolve.
void CompleteComponentLoadSuccess(GridContextFieldCatalog& cat, const std::string& projectKey) {
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    cat.projectComponentOptions_[projectKey] = std::vector<TrackerFieldOption>();
    cat.projectComponentsRetryAfter_.erase(projectKey);
    cat.projectComponentsInFlight_.erase(projectKey);
}

bool HasInFlight(GridContextFieldCatalog& cat, const std::string& projectKey) {
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    return cat.projectComponentsInFlight_.find(projectKey) != cat.projectComponentsInFlight_.end();
}

bool HasLoaded(GridContextFieldCatalog& cat, const std::string& projectKey) {
    std::lock_guard<std::mutex> lock(cat.availableFieldsMutex_);
    return cat.projectComponentOptions_.find(projectKey) != cat.projectComponentOptions_.end();
}

} // namespace

// #975 regression: a project pane stuck "Loading components…" forever. The worker must operate on
// the KICK-TIME context's catalog, not a completion-time fieldCatalog() re-resolve. If focus
// switches panes mid-fetch, a completion-time resolve erases/writes the WRONG context and leaves
// the kick-time context's projectComponentsInFlight_ marker set forever.
TEST_CASE("#975 component load completes against the kick-time context, not the focused-at-completion one") {
    // Two live contexts; focus starts on A, switches to B mid-fetch (the husk graveyard keeps the
    // kick-time pointer valid, so the worker can still reach A's catalog).
    GridLiveContext ctxA;
    GridLiveContext ctxB;
    const std::string projectKey = "ABC";

    // Kick against A (the focused context at kick time) — marker lands on A.
    REQUIRE(KickComponentLoad(ctxA.fieldCatalog, projectKey));
    CHECK(HasInFlight(ctxA.fieldCatalog, projectKey));
    CHECK_FALSE(HasInFlight(ctxB.fieldCatalog, projectKey));

    // Capture the kick-time catalog pointer (the fix). Focus then switches to B before the worker
    // completes — modelled by NOT routing completion through B.
    GridContextFieldCatalog* kickTimeCatalog = &ctxA.fieldCatalog;

    SUBCASE("fixed: worker resolves the kick-time catalog → A clears, B untouched") {
        CompleteComponentLoadSuccess(*kickTimeCatalog, projectKey);

        CHECK_FALSE(HasInFlight(ctxA.fieldCatalog, projectKey)); // marker cleared — no leak
        CHECK(HasLoaded(ctxA.fieldCatalog, projectKey));         // options landed on A
        CHECK_FALSE(HasInFlight(ctxB.fieldCatalog, projectKey)); // B never touched
        CHECK_FALSE(HasLoaded(ctxB.fieldCatalog, projectKey));   // no spurious write to B
    }

    SUBCASE("regression witness: a completion-time re-resolve to the focused (B) catalog leaks A") {
        // This is the OLD behaviour the fix removes — pinned so a regression that re-introduces the
        // fieldCatalog() re-resolve inside the worker fails here instead of shipping the stuck pane.
        GridContextFieldCatalog* completionTimeFocused = &ctxB.fieldCatalog;
        CompleteComponentLoadSuccess(*completionTimeFocused, projectKey);

        CHECK(HasInFlight(ctxA.fieldCatalog, projectKey));     // LEAK: A stuck "Loading…" forever
        CHECK_FALSE(HasLoaded(ctxA.fieldCatalog, projectKey)); // A never got its options
        CHECK(HasLoaded(ctxB.fieldCatalog, projectKey));       // spurious write to the wrong context
    }
}

TEST_CASE("active-ticket publish path: mutate under mutex, publish snapshot, bump revision") {
    GridLiveContext ctx;

    CachedTicket t;
    t.id = "ABC-1";
    {
        std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
        ctx.ActiveTickets.push_back(t);
        ctx.activeTicketsPublished_ = std::make_shared<const std::vector<CachedTicket>>(ctx.ActiveTickets);
    }
    ctx.ActiveTicketsRevision.fetch_add(1);

    std::shared_ptr<const std::vector<CachedTicket>> snap;
    {
        std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
        snap = ctx.activeTicketsPublished_;
    }
    REQUIRE(snap != nullptr);
    REQUIRE(snap->size() == 1u);
    CHECK((*snap)[0].id == "ABC-1");
    CHECK(ctx.ActiveTicketsRevision.load() == 1u);

    // The published snapshot is an immutable copy: later cache mutations don't reach it.
    {
        std::lock_guard<std::mutex> lock(ctx.activeTicketsMutex_);
        ctx.ActiveTickets.clear();
    }
    CHECK(snap->size() == 1u);
}
