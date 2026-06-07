// GridLiveContext seam tests — multi-grid foundation Slice 1a
// (docs/plans/active/multi-grid-tabs.md, ADR-0018).
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

#include <memory>
#include <mutex>
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
