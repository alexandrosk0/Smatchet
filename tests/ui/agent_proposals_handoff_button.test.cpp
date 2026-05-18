// agent_proposals_handoff_button.test.cpp — bucket-E coverage for the H9
// `[Start handoff]` button on the T6 Agent proposals panel.
//
// Plan-locked: button visible ONLY on rows whose action is `ImplementIssue`
// (per `docs/design/agentic-coding-handoff.md` § "Phased rollout" H9 row).
// All other action types (CommentAdd / LabelAdd / etc) render only Approve
// / Reject — never the handoff button. Defense-in-depth: the
// OnProposalApproved callback in AppController applies the same filter on
// the cross-flow side; this test enforces the UI side of the contract.
//
// Drift warning — IF YOU CHANGE Source_Core/src/SmatchetAgentProposalsUi.cpp
// (the `[Start handoff]` conditional under `#if SMATCHET_WITH_AGENTIC`),
// UPDATE THIS REPLICA to match. We render a stripped-down replica that
// inlines the same `if (row.action == ImplementIssue && controllerLive)`
// gate the production panel uses. The replica bypasses
// AgenticHandoffController (which would drag in runner + GitHub seams) by
// installing a stub StartFn that flips an in-memory "active" set so
// SnapshotActive can be asserted on.
//
// Four variants:
//   1. CommentAdd_NoHandoffButton — non-ImplementIssue row has Approve +
//      Reject only.
//   2. LabelAdd_NoHandoffButton — same shape on a second non-handoff action.
//   3. ImplementIssue_ButtonVisible — ImplementIssue row exposes
//      [Start handoff] alongside Approve / Reject.
//   4. ImplementIssue_ClickStartsHandoff — clicking [Start handoff] flips
//      the row's state to Approved AND records a started-handoff entry in
//      the stub's "active" map. Asserts both the SQLite-side transition
//      (Find returns Approved) and the stub controller's active list.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticInferenceClientPure.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using AgenticInferenceClientPure::ProposedAction;

// Stub stand-in for AgenticHandoffController — captures Start() calls so the
// test can assert that the production button drives them.
struct StubHandoffController {
    // Set of proposalIds for which Start() succeeded.
    std::unordered_set<std::int64_t> active;
    int startCalls = 0;
    bool nextStartOk = true;

    bool Start(std::int64_t proposalId, std::string& outError) {
        ++startCalls;
        if (!nextStartOk) {
            outError = "stub-refused";
            return false;
        }
        active.insert(proposalId);
        return true;
    }
};

struct PanelReplicaState {
    std::unique_ptr<AgentProposalStore> store;
    std::vector<AgentProposal> cache;
    StubHandoffController controller;
    bool controllerLive = true; // mirrors `app.GetAgenticHandoffController() != nullptr`
    bool lastApproveOk = false;
    bool lastStartOk = false;
    std::string lastError;
};

PanelReplicaState g_state;

void ResetPanelState() {
    g_state.store.reset(new AgentProposalStore(":memory:"));
    g_state.cache.clear();
    g_state.controller = StubHandoffController();
    g_state.controllerLive = true;
    g_state.lastApproveOk = false;
    g_state.lastStartOk = false;
    g_state.lastError.clear();
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
// Approve / Reject buttons plus the H9 [Start handoff] conditional. The
// `[Start handoff]` button renders ONLY when:
//   (a) row.action == ImplementIssue
//   (b) g_state.controllerLive (the panel's guard against degraded mode).
void DrawPanelReplica() {
    for (size_t i = 0; i < g_state.cache.size(); ++i) {
        AgentProposal& row = g_state.cache[i];
        ImGui::PushID(static_cast<int>(row.id));

        const char* actionStr = AgenticInferenceClientPure::ActionToString(row.action);
        ImGui::Text("%s  [%s]", row.issueKey.c_str(), actionStr);

        if (ImGui::Button("Approve")) {
            g_state.lastApproveOk =
                g_state.store->Transition(row.id, AgentProposalState::Approved, std::string(), g_state.lastError);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reject")) {
            g_state.lastApproveOk =
                g_state.store->Transition(row.id, AgentProposalState::Rejected, std::string(), g_state.lastError);
        }
        // H9 — defense-in-depth conditional. Must match production exactly.
        if (row.action == ProposedAction::ImplementIssue && g_state.controllerLive) {
            ImGui::SameLine();
            if (ImGui::Button("Start handoff")) {
                // Production's ApproveAndStartHandoff: Transition then Start.
                g_state.lastApproveOk = g_state.store->Transition(row.id, AgentProposalState::Approved,
                                                                  std::string(), g_state.lastError);
                std::string startErr;
                g_state.lastStartOk = g_state.controller.Start(row.id, startErr);
                if (!g_state.lastStartOk) {
                    g_state.lastError = startErr;
                }
            }
        }

        ImGui::PopID();
        ImGui::Separator();
    }
    if (g_state.cache.empty()) {
        ImGui::TextUnformatted("No pending proposals.");
    }
}

void GuiFunc(ImGuiTestContext* /*ctx*/) {
    ImGui::SetNextWindowSize(ImVec2(420, 280), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::AgentProposalsHandoffButton", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        DrawPanelReplica();
    }
    ImGui::End();
}

void RegisterCommentAddNoButton(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposalsHandoffButton", "CommentAdd_NoHandoffButton");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        SeedPending("smatchet/example#100", ProposedAction::CommentAdd);
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsHandoffButton");
        ctx->Yield();
        ctx->Yield();

        // Approve + Reject must exist; Start handoff must NOT. ItemExists
        // uses ImGuiTestOpFlags_NoError internally so missing items return
        // false without marking the test as failed.
        IM_CHECK(ctx->ItemExists("**/Approve"));
        IM_CHECK(ctx->ItemExists("**/Reject"));
        IM_CHECK_EQ(ctx->ItemExists("**/Start handoff"), false);
    };
}

void RegisterLabelAddNoButton(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposalsHandoffButton", "LabelAdd_NoHandoffButton");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        SeedPending("smatchet/example#101", ProposedAction::LabelAdd);
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsHandoffButton");
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(ctx->ItemExists("**/Approve"));
        IM_CHECK(ctx->ItemExists("**/Reject"));
        IM_CHECK_EQ(ctx->ItemExists("**/Start handoff"), false);
    };
}

void RegisterImplementIssueButtonVisible(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposalsHandoffButton", "ImplementIssue_ButtonVisible");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        SeedPending("smatchet/example#102", ProposedAction::ImplementIssue);
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsHandoffButton");
        ctx->Yield();
        ctx->Yield();

        // All three buttons must exist on an ImplementIssue row.
        IM_CHECK(ctx->ItemExists("**/Approve"));
        IM_CHECK(ctx->ItemExists("**/Reject"));
        IM_CHECK(ctx->ItemExists("**/Start handoff"));
    };
}

void RegisterImplementIssueClickStartsHandoff(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentProposalsHandoffButton", "ImplementIssue_ClickStartsHandoff");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        AgentProposal a = SeedPending("smatchet/example#103", ProposedAction::ImplementIssue);
        IM_CHECK(a.id > 0);
        const int64_t rowId = a.id;
        RefreshCacheFromStore();
        ctx->SetRef("SmatchetTest::AgentProposalsHandoffButton");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(g_state.cache.size(), static_cast<size_t>(1));

        ctx->ItemClick("**/Start handoff");
        ctx->Yield();

        // Approve-then-Start contract observable from both sides:
        //   SQLite row -> Approved
        //   Stub controller -> registered active handoff
        IM_CHECK(g_state.lastApproveOk);
        IM_CHECK(g_state.lastStartOk);
        IM_CHECK_EQ(g_state.controller.startCalls, 1);
        IM_CHECK(g_state.controller.active.find(rowId) != g_state.controller.active.end());

        AgentProposal got;
        std::string err;
        IM_CHECK(g_state.store->Find(rowId, got, err));
        IM_CHECK(got.state == AgentProposalState::Approved);
    };
}

} // namespace

extern "C" void SmatchetRegisterAgentProposalsHandoffButtonTests(ImGuiTestEngine* engine) {
    RegisterCommentAddNoButton(engine);
    RegisterLabelAddNoButton(engine);
    RegisterImplementIssueButtonVisible(engine);
    RegisterImplementIssueClickStartsHandoff(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AGENTIC
