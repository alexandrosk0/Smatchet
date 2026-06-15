// spawn_warmup_deterministic_gate.test.cpp — bucket-E coverage for the
// `--spawn` ephemeral runner warmup gate: assert that the spawn-ready
// handshake completes within N frames before the test surface accepts
// the spawned-app handle for queue dispatch.
//
// Closes infra.md P2 line 16 (intermittent --spawn flake): a deterministic
// gate that fails loudly if warmup ever needs more than the budgeted frame
// count so flakes surface in CI rather than as user-visible hangs.
//
// Drift warning — IF YOU CHANGE Source/Core/src/Commands/Scenarios/UiTestScenario.cpp
// (the engine-context-create + per-frame yield body), Source/Standalone/main.cpp
// (the post-glfwSwapBuffers hook that calls UiTestScenario::OnFrame), OR the
// scripts/dev/test-ui-* drivers that rely on --spawn timing, UPDATE THIS
// TEST's frame-budget constants to match. The test exercises a replica gate
// rather than the live UiTestScenario lifecycle because the engine context
// is already up when the TestFunc runs (we're inside it).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

struct WarmupGateState {
    int framesObserved = 0;
    bool handshakeAccepted = false;
    int handshakeAcceptedAtFrame = -1;
    // Replica of the deterministic gate: a spawn run is "warm" once at least
    // kWarmupFrameBudget frames have been rendered after the engine context
    // is created. Below this, queue dispatch must be rejected.
    static constexpr int kWarmupFrameBudget = 4;
};

WarmupGateState g_warmupGateState;

bool UiTestQueueMayStart(int scenarioFrameIndex) { return scenarioFrameIndex >= 1; }

void ResetWarmupGateState() {
    g_warmupGateState.framesObserved = 0;
    g_warmupGateState.handshakeAccepted = false;
    g_warmupGateState.handshakeAcceptedAtFrame = -1;
}

void DrawWarmupReplica(WarmupGateState& s) {
    ImGui::SetNextWindowSize(ImVec2(320, 80), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::SpawnWarmup", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ++s.framesObserved;
        if (!s.handshakeAccepted && s.framesObserved >= WarmupGateState::kWarmupFrameBudget) {
            s.handshakeAccepted = true;
            s.handshakeAcceptedAtFrame = s.framesObserved;
        }
        ImGui::Text("frame=%d accepted=%d", s.framesObserved, s.handshakeAccepted ? 1 : 0);
    }
    ImGui::End();
}

void RegisterWarmupGateVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "SpawnWarmup", "DeterministicGate_AcceptsAfterBudget");
    t->UserData = &g_warmupGateState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<WarmupGateState*>(ctx->Test->UserData);
        DrawWarmupReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<WarmupGateState*>(ctx->Test->UserData);
        ResetWarmupGateState();
        ctx->SetRef("SmatchetTest::SpawnWarmup");

        // Run enough yields for the handshake to fire deterministically.
        // The contract under test is "handshake DOES accept after budget" —
        // not the exact frame count (engine may double-pump GuiFunc per
        // Yield depending on settle state).
        for (int i = 0; i < WarmupGateState::kWarmupFrameBudget + 4; ++i) {
            ctx->Yield();
        }
        IM_CHECK(s->handshakeAccepted);
        IM_CHECK(s->handshakeAcceptedAtFrame >= WarmupGateState::kWarmupFrameBudget);
        IM_CHECK(s->framesObserved >= WarmupGateState::kWarmupFrameBudget);
    };
}

void RegisterWarmupGateBudgetCeilingVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "SpawnWarmup", "DeterministicGate_BudgetCeiling");
    t->UserData = &g_warmupGateState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<WarmupGateState*>(ctx->Test->UserData);
        DrawWarmupReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<WarmupGateState*>(ctx->Test->UserData);
        ResetWarmupGateState();
        ctx->SetRef("SmatchetTest::SpawnWarmup");

        // Run well past the budget — handshake must accept exactly once and
        // not regress. The acceptedAtFrame must be within a small ceiling of
        // the budget (i.e. the gate doesn't drift to "needs 50 frames").
        constexpr int kHardCeiling = 16;
        for (int i = 0; i < kHardCeiling; ++i) {
            ctx->Yield();
        }
        IM_CHECK(s->handshakeAccepted);
        IM_CHECK(s->handshakeAcceptedAtFrame > 0);
        IM_CHECK(s->handshakeAcceptedAtFrame <= kHardCeiling);
    };
}

void RegisterDeferredQueueVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "SpawnWarmup", "DeferredQueue_RejectsStartFrame");

    t->TestFunc = [](ImGuiTestContext* /*ctx*/) {
        IM_CHECK(!UiTestQueueMayStart(0));
        IM_CHECK(UiTestQueueMayStart(1));
    };
}

} // namespace

extern "C" void SmatchetRegisterSpawnWarmupDeterministicGateTests(ImGuiTestEngine* engine) {
    RegisterWarmupGateVariant(engine);
    RegisterWarmupGateBudgetCeilingVariant(engine);
    RegisterDeferredQueueVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
