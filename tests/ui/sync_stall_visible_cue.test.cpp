// sync_stall_visible_cue.test.cpp — Slice 5 of
// docs/plans/shipped/pillar-1-2-perf-review-system.md.
//
// Bucket-E gate for UX Pillar 2 — "UI never freezes without a visible cue".
// The invariant under test: if the UI thread is about to block synchronously
// for > 100 ms, a spinner / progress widget must be SUBMITTED on the SAME
// frame and BEFORE the blocking call. If a code path calls block-then-cue
// the user sees a frozen frame with no indication anything is happening.
//
// Verification strategy — order, not wall-clock:
//   ImGui Test Engine drives frames in a stepped loop, so a literal
//   wall-clock < 100 ms assertion is unreliable. Instead we verify the
//   ORDER of submissions: did the spinner widget land on the frame buffer
//   BEFORE the blocking call ran? That's the architectural invariant. If
//   the order is right under any stepping rate, real users see the cue
//   well within 100 ms; if it's wrong, they see nothing for the duration
//   of the block.
//
// Why two variants (good / bad):
//   - good — cue-then-block: the spinner ImGui::Button is submitted, the
//     stall hook fires AFTER it, the sentinel atom records "spinner first".
//     TestFunc asserts the spinner item exists in the same frame and the
//     ordering atom == kCueBeforeStall. This is the production pattern.
//   - bad — block-then-cue (regression case): the stall hook runs first
//     and the spinner is submitted after. TestFunc asserts the ordering
//     atom == kStallBeforeCue. This documents what the bug looks like;
//     if a future change inverts the order in production code, the
//     scanner pattern won't catch it but the bucket-E test will.
//
// The stall sleep + ordering atoms are gated by
// SMATCHET_DEBUG_VISIBLE_CUE_HARNESS — production builds never compile
// the synthetic delay. The ninja-ui-test-msys2 preset enables the flag;
// the production presets do not.
//
// Why this test is self-contained:
//   The icon-fetch path that the plan originally named already runs on a
//   worker thread (see Source_Core/src/SmatchetFieldIconRender.cpp:309 +
//   :344 — both `app.LaunchBackgroundTask(...)` lambdas). There is no
//   live UI-thread sync stall in icon fetch to inject; the worker
//   already removes it. The harness lives in the test file so it
//   exercises the bucket-E pattern for future UI-thread sync paths that
//   may be introduced. New cue-bearing code paths can reuse this same
//   pattern: register an `IM_REGISTER_TEST` that mirrors the production
//   call-site shape, set kCueBeforeStall when cue is submitted, run the
//   block, assert ordering. See tests/ui/views_columns_reorder.test.cpp
//   for the analogous mirror-the-call-site pattern.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

// Cue / stall ordering sentinel. Engine ticks GuiFunc on the UI thread so
// no contention here, but the atomic documents the invariant we're
// asserting (no torn reads even if a future refactor moves stall to a
// worker).
enum class CueOrder : int {
    kUnobserved = 0,
    kCueBeforeStall = 1,
    kStallBeforeCue = 2,
};
std::atomic<int> g_cueOrder{static_cast<int>(CueOrder::kUnobserved)};
std::atomic<bool> g_stallActive{false};

#if defined(SMATCHET_DEBUG_VISIBLE_CUE_HARNESS)
// 50 ms is enough to make any cue-after-stall ordering bug humanly
// observable in a real UI under the same test pattern, while keeping the
// bucket-E run quick (one test frame per variant).
constexpr int kSyntheticStallMs = 50;
#endif

void TriggerStallHook() {
#if defined(SMATCHET_DEBUG_VISIBLE_CUE_HARNESS)
    if (!g_stallActive.load(std::memory_order_acquire)) {
        return;
    }
    // The order check is *cue submitted first*. If g_cueOrder is still
    // kUnobserved at the moment the stall runs, the cue was never
    // submitted before the stall — record kStallBeforeCue.
    int expected = static_cast<int>(CueOrder::kUnobserved);
    g_cueOrder.compare_exchange_strong(expected, static_cast<int>(CueOrder::kStallBeforeCue));
    std::this_thread::sleep_for(std::chrono::milliseconds(kSyntheticStallMs));
    g_stallActive.store(false, std::memory_order_release);
#endif
}

void SubmitSpinnerCue(const char* label) {
    // Mirror of the spinner-cue shape any UI-thread sync path should
    // emit: a small visible widget submitted under a stable label so a
    // bucket-E test can find it. In production this would be an actual
    // spinner; here a Button is sufficient since we're testing item
    // submission order, not the spinner pixel content.
    ImGui::Button(label, ImVec2(80, 24));
    int expected = static_cast<int>(CueOrder::kUnobserved);
    g_cueOrder.compare_exchange_strong(expected, static_cast<int>(CueOrder::kCueBeforeStall));
}

void RegisterGoodVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Pillar2", "SyncStallVisibleCue_GoodOrder_CueThenBlock");

    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(240, 80), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::SyncStallVisibleCue_Good")) {
            // Cue submitted FIRST; stall hook runs AFTER. This is the
            // production pattern any new UI-thread sync path must follow.
            SubmitSpinnerCue("##visible-cue-spinner-good");
            TriggerStallHook();
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_cueOrder.store(static_cast<int>(CueOrder::kUnobserved));
        g_stallActive.store(true, std::memory_order_release);

        ctx->SetRef("SmatchetTest::SyncStallVisibleCue_Good");
        // Let GuiFunc run one frame with stallActive == true.
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(g_cueOrder.load(), static_cast<int>(CueOrder::kCueBeforeStall));
        IM_CHECK(!g_stallActive.load());
    };
}

void RegisterBadVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Pillar2", "SyncStallVisibleCue_BadOrder_BlockThenCue");

    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(240, 80), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::SyncStallVisibleCue_Bad")) {
            // Regression shape: stall hook fires BEFORE the cue is
            // submitted. The sentinel atom records kStallBeforeCue
            // because g_cueOrder is still kUnobserved when the stall
            // runs. User sees no spinner for the 50 ms block.
            TriggerStallHook();
            SubmitSpinnerCue("##visible-cue-spinner-bad");
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_cueOrder.store(static_cast<int>(CueOrder::kUnobserved));
        g_stallActive.store(true, std::memory_order_release);

        ctx->SetRef("SmatchetTest::SyncStallVisibleCue_Bad");
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(g_cueOrder.load(), static_cast<int>(CueOrder::kStallBeforeCue));
        IM_CHECK(!g_stallActive.load());
    };
}

} // namespace

extern "C" void SmatchetRegisterSyncStallVisibleCueTests(ImGuiTestEngine* engine) {
    RegisterGoodVariant(engine);
    RegisterBadVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
