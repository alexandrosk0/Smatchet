// agent_handoff_panel.test.cpp — bucket-E coverage for the H8 Agent
// handoffs panel (SmatchetAgentHandoffUi).
//
// Drift warning — IF YOU CHANGE Source_Core/src/SmatchetAgentHandoffUi.cpp
// (the per-row Cancel / Open / Submit-clarification block, the
// IsCancellable predicate, or the lastClarificationQuestion / prUrl /
// worktreeDir display fields), UPDATE THIS REPLICA to match. The replica
// drives the same conceptual button actions but bypasses the
// AgenticHandoffController (which depends on the runner + GitHub seams) to
// keep the test rig free of cpr / SQLite / git-worktree spawn.
//
// Three test cases mirror the three primary action paths from the H8 plan:
//   1. AwaitingUser_SubmitClarification_TransitionsState — clicking
//      "Submit clarification" records the reply against the in-memory stub
//      controller and the stub flips state from AwaitingUser to Running
//      (the production path does the same via runner.Resume()).
//   2. Running_CancelButton_FlipsCancelAtom — clicking "Cancel" raises the
//      cancel atom; the stub controller treats this as a state change to
//      Cancelled.
//   3. PrOpen_OpenPrButton_RecordsLaunch — clicking "Open PR" hits an
//      in-memory launch-recorder rather than ShellExecuteW so the test
//      stays headless. Asserts the URL was forwarded verbatim.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AGENTIC)

#include "HarnessRunState.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Stub mirror of AgenticHandoffController::ActiveHandoff — only the fields
// the panel reads. Keeping the stub local avoids dragging the controller
// header (which transitively depends on AgentProposalStore) into the test.
struct StubHandoff {
    std::int64_t proposalId = 0;
    std::string issueKey;
    std::string worktreeDir;
    std::string branchName;
    CodingHarness::RunState state = CodingHarness::RunState::Pending;
    std::string prUrl;
    std::string lastClarificationQuestion;
    std::int64_t startedAtSec = 0;
};

// Stub controller — captures the action calls so the test can assert.
struct StubController {
    std::vector<StubHandoff> rows;
    // Captured-action evidence per proposalId.
    std::unordered_map<std::int64_t, std::string> capturedReplies;
    std::unordered_map<std::int64_t, bool> cancelCalls;
    // Launch recorder — replaces ShellExecuteW so the test runs headless.
    std::vector<std::string> launchRecorder;

    StubHandoff* Find(std::int64_t id) {
        for (auto& r : rows) {
            if (r.proposalId == id) {
                return &r;
            }
        }
        return nullptr;
    }

    // Mirror AgenticHandoffController::ProvideClarification: success requires
    // AwaitingUser; on success the FSM advances back to Running.
    bool ProvideClarification(std::int64_t id, const std::string& reply, std::string& err) {
        StubHandoff* h = Find(id);
        if (h == nullptr) {
            err = "unknown handoff";
            return false;
        }
        if (h->state != CodingHarness::RunState::AwaitingUser) {
            err = "not awaiting clarification";
            return false;
        }
        capturedReplies[id] = reply;
        h->state = CodingHarness::RunState::Running;
        h->lastClarificationQuestion.clear();
        return true;
    }

    bool Cancel(std::int64_t id, std::string& err) {
        StubHandoff* h = Find(id);
        if (h == nullptr) {
            err = "unknown handoff";
            return false;
        }
        if (h->state == CodingHarness::RunState::Complete || h->state == CodingHarness::RunState::Failed ||
            h->state == CodingHarness::RunState::Cancelled) {
            err = "terminal state";
            return false;
        }
        cancelCalls[id] = true;
        h->state = CodingHarness::RunState::Cancelled;
        return true;
    }

    bool LaunchUrl(const std::string& url) {
        if (url.empty()) {
            return false;
        }
        launchRecorder.push_back(url);
        return true;
    }
};

// Replica of the panel's TU-static state.
struct PanelReplicaState {
    std::unique_ptr<StubController> controller;
    std::int64_t selectedId = 0;
    std::unordered_map<std::int64_t, std::vector<char>> replyBufs;
};

PanelReplicaState g_state;

void ResetPanelState() {
    g_state.controller.reset(new StubController());
    g_state.selectedId = 0;
    g_state.replyBufs.clear();
}

std::vector<char>& EnsureReplyBuf(std::int64_t id) {
    auto& buf = g_state.replyBufs[id];
    if (buf.empty()) {
        buf.assign(4096, '\0');
    }
    return buf;
}

bool IsCancellable(CodingHarness::RunState s) {
    return s == CodingHarness::RunState::Running || s == CodingHarness::RunState::AwaitingUser ||
           s == CodingHarness::RunState::PrOpen || s == CodingHarness::RunState::Iterating;
}

// Mirrors the per-row + detail-pane block from the production UI: header
// table → row buttons (Cancel / Open PR / Open WT) → selected detail pane
// with submit-clarification.
void DrawPanelReplica() {
    auto& ctrl = *g_state.controller;
    if (ctrl.rows.empty()) {
        ImGui::TextUnformatted("No active handoffs.");
        return;
    }
    for (size_t i = 0; i < ctrl.rows.size(); ++i) {
        StubHandoff& row = ctrl.rows[i];
        ImGui::PushID(static_cast<int>(row.proposalId));

        // One row per handoff — text label + the conditional action buttons.
        // No Selectable / SameLine pattern: the production UI uses a table
        // for the row layout; here we just need each button to be a uniquely
        // resolvable ImGui item so ctx->ItemClick can target it.
        ImGui::Text("row#%lld [%s]", static_cast<long long>(row.proposalId), CodingHarness::RunStateToString(row.state));

        if (IsCancellable(row.state)) {
            if (ImGui::Button("Cancel")) {
                std::string err;
                ctrl.Cancel(row.proposalId, err);
            }
        }
        if (!row.prUrl.empty()) {
            if (ImGui::Button("OpenPR")) {
                ctrl.LaunchUrl(row.prUrl);
            }
        }
        if (!row.worktreeDir.empty()) {
            if (ImGui::Button("OpenWT")) {
                ctrl.LaunchUrl(row.worktreeDir);
            }
        }

        // Inline clarification box when the row is AwaitingUser. The button
        // is drawn BEFORE the input so the test engine's path-based click
        // never has to negotiate with InputText focus capture. The reply text
        // is pulled from the per-row buffer at click-time; the test harness
        // pre-populates the buffer directly via EnsureReplyBuf rather than
        // typing into the input (which would compound the focus risk).
        if (row.state == CodingHarness::RunState::AwaitingUser) {
            ImGui::Text("question: %s", row.lastClarificationQuestion.c_str());
            std::vector<char>& buf = EnsureReplyBuf(row.proposalId);
            if (ImGui::Button("SubmitClarification")) {
                std::string err;
                ctrl.ProvideClarification(row.proposalId, std::string(buf.data()), err);
            }
            ImGui::InputTextMultiline("##reply", buf.data(), buf.size(),
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.0f));
        }

        ImGui::Separator();
        ImGui::PopID();
    }
}

void GuiFunc(ImGuiTestContext* /*ctx*/) {
    ImGui::SetNextWindowSize(ImVec2(560, 320), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::AgentHandoffPanel", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        DrawPanelReplica();
    }
    ImGui::End();
}

void SeedHandoff(std::int64_t id, const std::string& issueKey, CodingHarness::RunState state,
                 const std::string& prUrl, const std::string& question) {
    StubHandoff h;
    h.proposalId = id;
    h.issueKey = issueKey;
    h.state = state;
    h.prUrl = prUrl;
    h.lastClarificationQuestion = question;
    h.worktreeDir = ".claude/worktrees/agent-" + std::to_string(id);
    h.branchName = "agent/" + std::to_string(id) + "/test";
    h.startedAtSec = 1700000000 + id;
    g_state.controller->rows.push_back(h);
}

void RegisterSubmitClarificationVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentHandoff", "AwaitingUser_SubmitClarification_TransitionsState");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        SeedHandoff(42, "smatchet/example#5", CodingHarness::RunState::AwaitingUser, std::string(),
                    "Which date format?");
        // The replica renders the Submit-clarification button inline on the
        // AwaitingUser row, so no Selectable selection step is needed.
        ctx->SetRef("SmatchetTest::AgentHandoffPanel");
        ctx->Yield();
        ctx->Yield();

        // Pre-populate the reply buffer so the click submits non-empty
        // content. The InputTextMultiline replica writes to the same vector
        // EnsureReplyBuf returns, so we can stuff the answer text directly.
        std::vector<char>& buf = EnsureReplyBuf(42);
        const std::string answer = "ISO 8601";
        for (size_t i = 0; i < answer.size() && i + 1 < buf.size(); ++i) {
            buf[i] = answer[i];
        }
        if (answer.size() < buf.size()) {
            buf[answer.size()] = '\0';
        }

        ctx->ItemClick("**/SubmitClarification");
        ctx->Yield();

        IM_CHECK(g_state.controller->capturedReplies.count(42) == 1);
        IM_CHECK_STR_EQ(g_state.controller->capturedReplies[42].c_str(), "ISO 8601");
        StubHandoff* h = g_state.controller->Find(42);
        IM_CHECK(h != nullptr);
        IM_CHECK(h->state == CodingHarness::RunState::Running);
    };
}

void RegisterCancelVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentHandoff", "Running_CancelButton_FlipsCancelAtom");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        SeedHandoff(43, "smatchet/example#7", CodingHarness::RunState::Running, std::string(), std::string());
        ctx->SetRef("SmatchetTest::AgentHandoffPanel");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("**/Cancel");
        ctx->Yield();

        IM_CHECK(g_state.controller->cancelCalls.count(43) == 1);
        IM_CHECK(g_state.controller->cancelCalls[43]);
        StubHandoff* h = g_state.controller->Find(43);
        IM_CHECK(h != nullptr);
        IM_CHECK(h->state == CodingHarness::RunState::Cancelled);
    };
}

void RegisterOpenPrVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AgentHandoff", "PrOpen_OpenPrButton_RecordsLaunch");
    t->GuiFunc = GuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetPanelState();
        SeedHandoff(44, "smatchet/example#12", CodingHarness::RunState::PrOpen,
                    "https://github.com/example/repo/pull/123", std::string());
        ctx->SetRef("SmatchetTest::AgentHandoffPanel");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("**/OpenPR");
        ctx->Yield();

        IM_CHECK_EQ(g_state.controller->launchRecorder.size(), static_cast<size_t>(1));
        IM_CHECK_STR_EQ(g_state.controller->launchRecorder[0].c_str(),
                        "https://github.com/example/repo/pull/123");
    };
}

} // namespace

extern "C" void SmatchetRegisterAgentHandoffPanelTests(ImGuiTestEngine* engine) {
    RegisterSubmitClarificationVariant(engine);
    RegisterCancelVariant(engine);
    RegisterOpenPrVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AGENTIC
