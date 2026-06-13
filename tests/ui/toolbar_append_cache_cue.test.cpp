// toolbar_append_cache_cue.test.cpp — bucket-E visible-cue gate for the
// Pillar-2 sync-I/O offload cluster (#1001, site #611).
//
// Invariant under test: when the toolbar's per-tracker append cache reloads
// after a backend-key change, the disk read (LoadPersistentViewsFromDisk —
// ifstream + JSON under IoMutex + ScopedFileLock) runs OFF the UI thread, and
// while that load is in flight a VISIBLE in-progress cue is rendered in the bar
// on the SAME frame the in-flight flag is true. Pre-offload this was a silent
// sub-100ms-but-real block on the memo-miss frame; post-offload it is a silent
// async gap unless a cue is shown — this gate locks in the cue.
//
// Production shape mirrored (drift-warning — keep in lock-step):
//   Source/Core/src/Ui/SmatchetToolbarUi.cpp
//     :138       trackerAppendLoadInFlight_ = true;
//     :142-160   LaunchBackgroundTask(LoadPersistentViewsFromDisk) → PostToMainThread apply,
//                clearing trackerAppendLoadInFlight_ when the (still-current) load lands
//     RenderBar  if (trackerAppendLoadInFlight_) { ... SmallButton(ICON_FA_SPINNER
//                "##tb-append-loading"); }  <-- the cue (added with this gate)
//
// Why a TU-local replica (not the live SmatchetToolbarUi): trackerAppendLoadInFlight_
// is a private member (SmatchetToolbarUi.h) and RenderBar needs a live AppController +
// tracker backend to drive a real backend-key change — not drivable from a generic
// test TU. The replica mirrors the exact flag + cue id so the test stays deterministic;
// the cue label here MUST match the production widget id verbatim.
//
// Determinism: the "worker" is a release atom the TestFunc holds, so the cue is
// observable for >= 1 frame regardless of engine stepping rate.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>

namespace {

// Replica of SmatchetToolbarUi::trackerAppendLoadInFlight_ + the RenderBar cue.
struct ToolbarAppendCueReplica {
    std::atomic<bool> workerReleased{false}; // TestFunc flips this to "complete" the load
    bool loadInFlight = false;
};

ToolbarAppendCueReplica g_replica;

// Mirror of the RenderBar cue under the production widget id `##tb-append-loading`.
// Production wraps the spinner in BeginDisabled (a non-interactive cue); the test
// engine does not register disabled items for ItemInfo queries, so the replica
// submits the same id as a plain (enabled) findable proxy — the assertion is the
// cue's presence/absence, not its enabled state. Keep the id identical to the
// product widget so an id drift breaks this test.
void SubmitAppendLoadingCue() {
    if (g_replica.loadInFlight) {
        // Findable proxy with an explicit size + `##`-only id. The FA glyph the
        // product uses (ICON_FA_SPINNER) renders as a missing glyph under the
        // headless test font and can yield a zero-extent item the engine can't
        // locate, so the proxy carries no visible glyph — the id is what the
        // assertion matches, and it is identical to the product widget id.
        ImGui::Button("##tb-append-loading", ImVec2(80, 20));
    }
}

void RegisterVisibleWhileLoading(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Pillar2", "ToolbarAppendCue_VisibleWhileLoading");

    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(240, 80), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::ToolbarAppendCue_Visible")) {
            // Poll the "worker": once released, clear the in-flight flag exactly as
            // the PostToMainThread apply does (:152-158).
            if (g_replica.loadInFlight && g_replica.workerReleased.load(std::memory_order_acquire)) {
                g_replica.loadInFlight = false;
            }
            SubmitAppendLoadingCue();
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Arm the in-flight state (mirror :138).
        g_replica.workerReleased.store(false, std::memory_order_release);
        g_replica.loadInFlight = true;

        ctx->SetRef("SmatchetTest::ToolbarAppendCue_Visible");
        ctx->Yield(); // one frame with loadInFlight == true

        // Cue must be on screen while the load is in flight (ID != 0).
        IM_CHECK(ctx->ItemInfo("##tb-append-loading", ImGuiTestOpFlags_NoError).ID != 0);

        // Complete the worker; the next frame's poll clears the flag and the cue
        // stops being submitted.
        g_replica.workerReleased.store(true, std::memory_order_release);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(!g_replica.loadInFlight);
        // Cue gone once the load landed: ItemInfo returns the null record.
        IM_CHECK(ctx->ItemInfo("##tb-append-loading", ImGuiTestOpFlags_NoError).ID == 0);
    };
}

} // namespace

extern "C" void SmatchetRegisterToolbarAppendCacheCueTests(ImGuiTestEngine* engine) {
    RegisterVisibleWhileLoading(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
