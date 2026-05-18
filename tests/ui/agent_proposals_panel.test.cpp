// agent_proposals_panel.test.cpp — bucket-E coverage for the T6 Agent
// proposals panel (SmatchetAgentProposalsUi).
//
// Drift warning — IF YOU CHANGE Source_Core/src/SmatchetAgentProposalsUi.cpp
// (the Approve / Reject button block or the Pending-row refresh path),
// UPDATE THIS REPLICA to match. The replica drives the same
// AgentProposalStore::Transition calls but bypasses the AppController to keep
// the test rig free of cpr / GitHub / inference.
//
// Four variants:
//   1. Empty_ShowsEmptyState — store has no Pending rows; the "no proposals"
//      message renders.
//   2. ListsPending_ShowsIssueKeyAndAction — single Pending row visible with
//      its issueKey + action discriminator.
//   3. Approve_TransitionsToApproved — clicking Approve calls
//      AgentProposalStore::Transition(Approved); the row leaves the Pending
//      set and the store's row state == Approved.
//   4. Reject_TransitionsToRejected — symmetric on the Reject side.
//
// Replica scope — the test mirrors the per-row Approve / Reject column block
// from SmatchetAgentProposalsUi.cpp (the button-action -> Transition path),
// not the CollapsingHeader presentation or the 1 Hz refresh cadence. The
// cadence is exercised manually via RefreshFromStore. Production code uses
// a TU-static cache; the replica keeps the same vector at TU scope.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using AgenticInferenceClientPure::ProposedAction;

// Replica of SmatchetAgentProposalsUi.cpp internal state. Per-test reset
// clears the vector + reseats the in-memory store ptr.
struct PanelReplicaState {
    std::unique_ptr<AgentProposalStore> store;
    std::vector<AgentProposal> cache;
    bool lastTransitionOk = false;
    std::string lastTransitionError;
};

PanelReplicaState g_state;

void ResetPanelState() {
    g_state.store.reset(new AgentProposalStore(":memory:"));
    g_state.cache.clear();
    g_state.lastTransitionOk = false;
    g_state.lastTransitionError.clear();
}

void RefreshCacheFromStore() {
    AgentProposalStore::Filter filter;
    filter.states.push_back(AgentProposalState::Pending);
    std::string err;
    std::vector<AgentProposal> rows;
    if (g_state.store->Query(filter, rows, err)) {
        std::sort(rows.begin(), rows.end(),
                  [](const AgentProposal& a, const AgentProposal& b) { return a.createdAtSec > b.createdAtSec; });
        g_state.cache = std::move(rows);
    } else {
        g_state.cache.clear();
    }
}

AgentProposal SeedPending(const std::string& issueKey, ProposedAction action) {
    AgentProposal p;
    p.sourceTracker = "github";
    p.issueKey = issueKey;
    p.action = action;
    p.rationale = "stub rationale";
    p.payload = nlohmann::json::object();
    std::string err;
    if (g_state.store->Insert(p, err)) {
        return p;
    }
    return AgentProposal{};
}

// Mirrors SmatchetAgentProposalsUi.cpp's per-row UI block — header label +
// Approve / Reject buttons. We render an ItemId per row keyed off the proposal
// id so ItemClick can target a single button unambiguously.
void DrawPanelReplica() {
    for (size_t i = 0; i < g_state.cache.size(); ++i) {
        AgentProposal& row = g_state.cache[i];
        ImGui::PushID(static_cast<int>(row.id));

        const char* actionStr = AgenticInferenceClientPure::ActionToString(row.action);
        ImGui::Text("%s  [%s]", row.issueKey.c_str(), actionStr);

        if (ImGui::Button("Approve")) {
            g_state.lastTransitionOk = g_state.store->Transition(row.id, AgentProposalState::Approved, std::string(),
                                                                  g_state.lastTransitionError);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reject")) {
            g_state.lastTransitionOk = g_state.store->Transition(row.id, AgentProposalState::Rejected, std::string(),
                                                                  g_state.lastTransitionError);
        }

        ImGui::PopID();
        ImGui::Separator();
    }
    if (g_state.cache.empty()) {
        ImGui::TextUnformatted("No pending proposals.");
    }
}

void GuiFunc(ImGuiTestContext* /*ctx*/) {
    ImGui::SetNextWindowSize(ImVec2(420, 240), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::AgentProposalsPanel", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        DrawPanelReplica();
    }
    ImGui::End();
}

void RegisterEmptyVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposals", "Empty_ShowsEmptyState");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsPanel");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(g_state.cache.empty());
    };
}

void RegisterListPendingVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposals", "ListsPending_ShowsIssueKeyAndAction");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        AgentProposal a = SeedPending("smatchet/example#42", ProposedAction::CommentAdd);
        IM_CHECK(a.id > 0);
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsPanel");
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(g_state.cache.size(), static_cast<size_t>(1));
        IM_CHECK_STR_EQ(g_state.cache[0].issueKey.c_str(), "smatchet/example#42");
        IM_CHECK(g_state.cache[0].action == ProposedAction::CommentAdd);
    };
}

void RegisterApproveVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposals", "Approve_TransitionsToApproved");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        AgentProposal a = SeedPending("smatchet/example#43", ProposedAction::LabelAdd);
        IM_CHECK(a.id > 0);
        const int64_t rowId = a.id;
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsPanel");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_state.cache.size(), static_cast<size_t>(1));

        ctx->ItemClick("**/Approve");
        ctx->Yield();
        IM_CHECK(g_state.lastTransitionOk);

        // Verify the persisted state. Find ignores the state filter.
        AgentProposal got;
        std::string err;
        IM_CHECK(g_state.store->Find(rowId, got, err));
        IM_CHECK(got.state == AgentProposalState::Approved);

        // Pending query no longer returns the row.
        RefreshCacheFromStore();
        IM_CHECK(g_state.cache.empty());
    };
}

void RegisterRejectVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposals", "Reject_TransitionsToRejected");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        AgentProposal a = SeedPending("smatchet/example#44", ProposedAction::StateTransition);
        IM_CHECK(a.id > 0);
        const int64_t rowId = a.id;
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsPanel");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_state.cache.size(), static_cast<size_t>(1));

        ctx->ItemClick("**/Reject");
        ctx->Yield();
        IM_CHECK(g_state.lastTransitionOk);

        AgentProposal got;
        std::string err;
        IM_CHECK(g_state.store->Find(rowId, got, err));
        IM_CHECK(got.state == AgentProposalState::Rejected);

        RefreshCacheFromStore();
        IM_CHECK(g_state.cache.empty());
    };
}

} // namespace

extern "C" void SmatchetRegisterAgentProposalsPanelTests(ImGuiTestEngine* engine) {
    RegisterEmptyVariant(engine);
    RegisterListPendingVariant(engine);
    RegisterApproveVariant(engine);
    RegisterRejectVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AGENTIC
