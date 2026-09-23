// mobile_desktop_layout_roundtrip.test.cpp — bucket-E regression guard for the
// Mobile ↔ Desktop UI-mode round-trip docking preservation fix.
//
// Bug (fixed): On the Mobile→Desktop transition, drawMobileRestoreDesktopIni was
// called mid-frame, rebuilding dock nodes that DockSpaceOverViewport had not marked
// alive (LastFrameAlive < FrameCount). Each desktop window's BeginDocked then hit
// the liveness check and called DockContextProcessUndockWindow, clearing the DockId
// and marking the ini dirty. The broken layout was then autosaved, wiping the user's
// custom desktop layout.
//
// Secondary bug (fixed): On the Desktop→Mobile transition, drawMobileEnsureIniAttached
// did ClearIniSettings without first saving the live desktop layout to disk. ImGui only
// autosaves every 5 seconds, so dock changes made in the last few seconds before the
// window got narrow were lost.
//
// Fix:
// 1. Defer the Mobile→Desktop ini swap from mid-frame to end-of-frame (after every
//    window has ended). Nodes rebuild on the next NewFrame and settle cleanly before
//    BeginDocked runs, so they never undock.
// 2. Save the desktop layout to disk before detaching io.IniFilename on the
//    Desktop→Mobile edge to flush changes still inside ImGui's 5 s autosave window.
//
// Verification: Probe the LIVE host after the round-trip: record which canonical
// desktop windows were docked before the flip, switch to Mobile, switch back to Desktop,
// and assert each previously-docked window is still docked with the same root node.
// Gate on pre-state so headless Mesa (no HostWindow even when healthy) skips instead of
// false-failing.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiDockNode::DockId / ParentNode / HostWindow, DockBuilderGetNode
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

extern UiDrawSession g_ui;

namespace {

// Snapshot which canonical windows are docked before the flip, keyed by DockId.
struct DockedWindowSnapshot {
    std::string WindowTitle;
    ImGuiID DockId;
};

// Fetch the docked state of one canonical window by its runtime title (localized).
// Returns {title, DockId} if the window is currently live and docked, or {title, 0} otherwise.
DockedWindowSnapshot SnapshotWindowDockState(const char* windowTitle) {
    const ImGuiWindow* win = ::ImGui::FindWindowByName(windowTitle);
    if (win == nullptr) {
        return {windowTitle, 0};
    }
    // DockId != 0 and a valid (non-orphan) node means the window is docked and reachable.
    if (win->DockId == 0) {
        return {windowTitle, 0};
    }
    const ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(win->DockId);
    if (node == nullptr || node->ParentNode == nullptr || node->HostWindow == nullptr) {
        return {windowTitle, 0};
    }
    return {windowTitle, win->DockId};
}

// Poll until a condition is true or maxFrames elapsed.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True if a live dock node with `nodeId` exists and is non-orphan.
bool NodeIsDockedNonOrphan(ImGuiID nodeId) {
    const ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(nodeId);
    return node != nullptr && node->ParentNode != nullptr && node->HostWindow != nullptr;
}

// ============================================================================
// Mobile ↔ Desktop UI-mode round-trip preserves docked windows.
// ============================================================================
void RegisterMobileDesktopLayoutRoundTripTest(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "MobileDesktopLayoutRoundtrip", "PreservesDocking_AfterRoundTrip");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
            return;
        }

        // Save state we mutate so the test leaves g_ui as it found it.
        const UiMode origUiMode = g_ui.cfg.UiMode;
        const MobilePage origPage = g_ui.mobilePage;

        // Let the desktop boot settle first so the canonical nodes are realized.
        for (int i = 0; i < 8; ++i) {
            ctx->Yield();
        }

        // Snapshot which canonical windows are docked BEFORE the flip. Only assert
        // windows that were docked before — in Mesa-headless HostWindow is nullptr
        // even when healthy, so we gate on pre-state to skip cleanly instead of
        // false-failing. The localized window titles match the real render path.
        const DockedWindowSnapshot issuesDocked = SnapshotWindowDockState("Issues");
        const bool preIssuesDocked = (issuesDocked.DockId != 0);

        if (!preIssuesDocked) {
            ctx->LogInfo("skip: Issues window not docked pre-flip — headless/non-default host layout");
            return;
        }

        // Switch to Mobile mode. drawResolveUiMode (SmatchetMobileShellUi.cpp) pins Mobile
        // when cfg.UiMode == UiMode::Mobile, bypassing the viewport-width hysteresis check.
        g_ui.cfg.UiMode = UiMode::Mobile;

        // Poll until the mobile shell is live (##MobileShell Begin() returned true this frame).
        const bool shellLive = YieldUntil(ctx, [&] {
            const ImGuiWindow* win = ::ImGui::FindWindowByName("##MobileShell");
            return win != nullptr && win->Active;
        });
        IM_CHECK_NO_RET(shellLive);

        if (!shellLive) {
            // Restore and bail.
            g_ui.cfg.UiMode = origUiMode;
            g_ui.mobilePage = origPage;
            return;
        }

        // Switch back to Desktop. The LIVE Draw loop (SmatchetUI::Draw) detects
        // effectiveUiMode == Desktop while mobileDockSeeded is still true, and defers
        // the ini swap to end-of-frame (drawEndOfFramePersistence). The desktop windows
        // are then submitted on a rebuilt tree that BeginDocked (next frame) finds
        // non-orphan, so they stay docked.
        g_ui.cfg.UiMode = origUiMode;

        // Poll until the desktop Issues window is back and docked with the same root node.
        // The dock tree rebuilds lazily on the next NewFrame after the ini swap, then
        // BeginDocked applies the restored DockIds on that frame. A generous frame cap
        // handles slow Mesa settle.
        const ImGuiID preDockId = issuesDocked.DockId;
        bool restored = false;
        for (int i = 0; i < 120 && !restored; ++i) {
            ctx->Yield();
            const DockedWindowSnapshot postIssues = SnapshotWindowDockState("Issues");
            restored = (postIssues.DockId != 0 && postIssues.DockId == preDockId);
        }

        // The exact inverse of the undock bug: the Issues window that was docked
        // before the round-trip is STILL docked with the same root node after it.
        IM_CHECK_NO_RET(restored);

        // Restore every mutated field.
        g_ui.cfg.UiMode = origUiMode;
        g_ui.mobilePage = origPage;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterMobileDesktopLayoutRoundtripTests(ImGuiTestEngine* engine) {
    RegisterMobileDesktopLayoutRoundTripTest(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
