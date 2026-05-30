// agent_proposal_store_sqlite.test.cpp — bucket-E coverage for the
// SQLite-backed AgentProposalStore UI flow. Per the slice-9 disposition
// (docs/plans/shipped/autonomous-debugging-no-creds.md § Slice 9), the "lane
// sub-rig" framing was wrong; the existing `tests/support/SqliteMemFixture.h`
// covers the SQLite-tempfile-lifecycle need directly with no new harness.
//
// The shipped AgentProposalStore production type does not exist yet (the
// agentic-flow plan ships it in a later slice); until it lands, this TU
// covers the **lifecycle contract** the future store will inherit from
// `LocalCacheManager`: fresh ":memory:" DB per TestFunc → no state-leak
// between sequential bucket-E tests; schema-init idempotent on Reopen();
// metadata flag round-trips through save → load.
//
// Drift warning — IF YOU CHANGE Source/Core/include/LocalCacheManager.h
// (the HasCacheMetaFlag / SetCacheMetaFlag pair we use as the proxy
// store surface), tests/support/SqliteMemFixture.h (the SqliteMemFixture
// constructor / Reopen contract), OR when the production AgentProposalStore
// ships (replace LocalCacheManager calls below with the new store's
// equivalents), UPDATE THIS TEST to match.

// Note: the production AgentProposalStore type ships in a later
// agentic-flow slice. Until then this TU exercises the SqliteMemFixture +
// LocalCacheManager lifecycle contract that the future store will inherit.
// The plan's per-test gating instruction (§ Slice 9) called for
// SMATCHET_WITH_AGENTIC — but that CMake option was removed in PR #356
// (github-tracker-backend.md "agentic ripout"), so the gate would no-op the
// test forever. Gate on SMATCHET_BUILD_UI_TESTS only so the contract stays
// covered; replace with the real store's gate once it lands.
#if defined(SMATCHET_BUILD_UI_TESTS)

#include "LocalCacheManager.h"
#include "SqliteMemFixture.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>

namespace {

struct ProposalStoreState {
    // The fixture owns its own LocalCacheManager — declared here as a fresh
    // instance per TestFunc so the SqliteMemFixture's ":memory:" lifetime
    // contract is exercised directly.
    bool freshDbObserved = false;
    bool flagWrittenObserved = false;
    bool reopenIdempotent = false;
};

ProposalStoreState g_proposalStoreState;

void ResetProposalStoreState() {
    g_proposalStoreState.freshDbObserved = false;
    g_proposalStoreState.flagWrittenObserved = false;
    g_proposalStoreState.reopenIdempotent = false;
}

void DrawProposalStoreReplica(const ProposalStoreState& s) {
    // Tiny shell so the bucket-E surface has a window to anchor against;
    // the assertion happens via the fixture, not the UI.
    ImGui::SetNextWindowSize(ImVec2(280, 80), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::ProposalStore", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("fresh=%d wrote=%d reopened=%d", s.freshDbObserved ? 1 : 0, s.flagWrittenObserved ? 1 : 0,
                    s.reopenIdempotent ? 1 : 0);
    }
    ImGui::End();
}

void RegisterFreshDbPerTestVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposalStore", "SqliteFresh_PerTestIsolation");
    t->UserData = &g_proposalStoreState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ProposalStoreState*>(ctx->Test->UserData);
        DrawProposalStoreReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ProposalStoreState*>(ctx->Test->UserData);
        ResetProposalStoreState();

        smatchet_tests::SqliteMemFixture fixture;
        LocalCacheManager& cache = fixture.Ref();

        const std::string key = "agent_proposal_store_sqlite::probe_key";
        // Fresh DB → flag MUST NOT exist.
        s->freshDbObserved = !cache.HasCacheMetaFlag(key);
        IM_CHECK(s->freshDbObserved);

        cache.SetCacheMetaFlag(key);
        s->flagWrittenObserved = cache.HasCacheMetaFlag(key);
        IM_CHECK(s->flagWrittenObserved);

        ctx->Yield();
        ctx->Yield();
    };
}

void RegisterReopenIdempotentVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposalStore", "SqliteFresh_ReopenIdempotent");
    t->UserData = &g_proposalStoreState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ProposalStoreState*>(ctx->Test->UserData);
        DrawProposalStoreReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ProposalStoreState*>(ctx->Test->UserData);
        ResetProposalStoreState();

        smatchet_tests::SqliteMemFixture fixture;
        const std::string key = "agent_proposal_store_sqlite::reopen_key";
        fixture.Ref().SetCacheMetaFlag(key);
        IM_CHECK(fixture.Ref().HasCacheMetaFlag(key));

        // Reopen drops the ":memory:" DB + reopens fresh — the schema-init
        // path must be idempotent (no exception, no UNIQUE-collision on
        // schema-version row). Post-reopen the flag is gone (in-memory DBs
        // don't persist across connections).
        fixture.Reopen();
        s->reopenIdempotent = !fixture.Ref().HasCacheMetaFlag(key);
        IM_CHECK(s->reopenIdempotent);

        ctx->Yield();
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterAgentProposalStoreSqliteTests(ImGuiTestEngine* engine) {
    RegisterFreshDbPerTestVariant(engine);
    RegisterReopenIdempotentVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
