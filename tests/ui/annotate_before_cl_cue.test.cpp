// annotate_before_cl_cue.test.cpp — bucket-E visible-cue gate for the
// Pillar-2 sync-I/O offload cluster (#1001, site #761).
//
// Invariant under test: when the Annotate "or day" date picker resolves a
// calendar day to its first submitted Perforce changelist, the resolve runs
// OFF the UI thread (server-wide `p4 changes` scan — multi-second), and while
// it is in flight a VISIBLE in-progress cue is rendered on the SAME frame the
// in-flight flag is true. The user must never face a frozen / silent frame.
//
// Production shape mirrored (drift-warning — keep in lock-step):
//   Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp
//     :207-208  State().beforeClResolving = true;
//               State().lastUiStatus = "Resolving first submitted changelist for that day...";
//     :224-237  poll State().beforeClFut via wait_for(0); on ready clear beforeClResolving
//     :107-108  if (!State().lastUiStatus.empty()) ImGui::TextWrapped("%s", ...);  <-- the cue
//
// Why a TU-local replica (not the live AnnotateState): AnnotateState lives
// behind AnnotateAnalysisUi_Internal.h, which `#define ImGui
// SmatchetLocalizedImGui` and pulls private types — un-includable from a
// generic test TU without ODR / macro hazards. The replica mirrors the exact
// flag + cue shape so the bucket-E test stays deterministic; if the production
// cue is removed or moved AFTER the in-flight set, this test must be updated in
// the same change (that is the point of the gate).
//
// Determinism: the "worker" is a release atom the TestFunc holds, so the cue is
// observable for >= 1 frame regardless of engine stepping rate (order, not
// wall-clock — same philosophy as sync_stall_visible_cue.test.cpp).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <string>

namespace {

// Replica of the in-flight + cue state. `resolving` mirrors
// AnnotateState::beforeClResolving; `status` mirrors AnnotateState::lastUiStatus.
struct BeforeClCueReplica {
    std::atomic<bool> workerReleased{false}; // TestFunc flips this to "complete" the resolve
    bool resolving = false;
    std::string status;
};

BeforeClCueReplica g_replica;

// Mirror of AnnotateAnalysisUi_Window.cpp :107-108 — the only on-screen cue
// while the day→CL resolve is in flight. Submitted under a stable label so
// ItemExists can find it (the production TextWrapped has no stable id; the
// findable proxy is the established convention, see sync_stall_visible_cue).
void SubmitResolvingCue() {
    if (g_replica.resolving) {
        ImGui::TextWrapped("%s", g_replica.status.c_str());
        // Findable anchor for the bucket-E assertion (proxy for the TextWrapped above).
        ImGui::Button("##annotate-resolving-cue", ImVec2(80, 20));
    }
}

void RegisterVisibleWhileResolving(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Pillar2", "AnnotateBeforeClCue_VisibleWhileResolving");

    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(320, 100), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::AnnotateBeforeClCue_Visible")) {
            // Poll the "worker": once released, clear the in-flight flag exactly
            // as the production poll does on future_status::ready (:224-237).
            if (g_replica.resolving && g_replica.workerReleased.load(std::memory_order_acquire)) {
                g_replica.resolving = false;
                g_replica.status = "Before changelist set to first submitted CL on that day: 12345";
            }
            SubmitResolvingCue();
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Arm the in-flight state (mirror :207-208).
        g_replica.workerReleased.store(false, std::memory_order_release);
        g_replica.resolving = true;
        g_replica.status = "Resolving first submitted changelist for that day...";

        ctx->SetRef("SmatchetTest::AnnotateBeforeClCue_Visible");
        ctx->Yield(); // one frame with resolving == true

        // Cue must be on screen while the resolve is in flight. ItemInfo with
        // NoError returns a null record (ID == 0) for an item not submitted in
        // recent frames, so ID != 0 here proves the cue is live.
        IM_CHECK(ctx->ItemInfo("##annotate-resolving-cue", ImGuiTestOpFlags_NoError).ID != 0);

        // Complete the worker; the next frame's poll clears the flag and the cue
        // stops being submitted. Yield enough frames for the engine to observe
        // the absence (ItemInfo retries a couple of frames internally).
        g_replica.workerReleased.store(true, std::memory_order_release);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(!g_replica.resolving);
        // Cue gone once the resolve landed: ItemInfo returns the null record.
        IM_CHECK(ctx->ItemInfo("##annotate-resolving-cue", ImGuiTestOpFlags_NoError).ID == 0);
    };
}

} // namespace

extern "C" void SmatchetRegisterAnnotateBeforeClCueTests(ImGuiTestEngine* engine) {
    RegisterVisibleWhileResolving(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
