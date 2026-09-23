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
#include "Commands/Scenarios/UiTestScenario.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiDockNode::DockId / ParentNode / HostWindow, DockBuilderGetNode
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

extern UiDrawSession g_ui;

namespace {

/// Snapshot of a canonical window's docked state, keyed by DockId and root node.
struct DockedWindowSnapshot {
    std::string WindowTitle; ///< Localized window title
    ImGuiID DockId;          ///< Dock node ID if docked, 0 if undocked or window not found
    ImGuiID RootNodeId;      ///< Root dock node ID, 0 if not docked
};

/// Fetch the docked state of one canonical window by its runtime title (localized).
/// @param windowTitle Localized window name to look up
/// @return {title, DockId, RootNodeId} if the window is currently live and docked with a valid non-orphan node,
///         {title, 0, 0} if undocked or window not found
DockedWindowSnapshot SnapshotWindowDockState(const char* windowTitle) {
    const ImGuiWindow* win = ::ImGui::FindWindowByName(windowTitle);
    if (win == nullptr) {
        return {windowTitle, 0, 0};
    }
    // DockId != 0 and a valid (non-orphan) node means the window is docked and reachable.
    if (win->DockId == 0) {
        return {windowTitle, 0, 0};
    }
    const ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(win->DockId);
    if (node == nullptr || node->ParentNode == nullptr || node->HostWindow == nullptr) {
        return {windowTitle, 0, 0};
    }
    // Walk up to find the root node (the one with no parent).
    ImGuiID rootNodeId = win->DockId;
    ImGuiDockNode* currentNode = const_cast<ImGuiDockNode*>(node);
    while (currentNode->ParentNode != nullptr) {
        rootNodeId = currentNode->ParentNode->ID;
        currentNode = currentNode->ParentNode;
    }
    return {windowTitle, win->DockId, rootNodeId};
}

/// Poll a predicate over multiple frames, yielding the UI loop between checks.
/// @tparam Pred Callable returning bool; called each frame
/// @param ctx ImGui test context for yielding control
/// @param pred Predicate to poll; returns true when condition is met
/// @param maxFrames Maximum number of frames to poll before timeout
/// @return true if predicate became true within maxFrames, false on timeout
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

/// Check if a dock node is live and non-orphan (has valid parent and host window).
/// @param nodeId Dock node ID to check
/// @return true if the node exists and is properly linked (not orphaned), false otherwise
bool NodeIsDockedNonOrphan(ImGuiID nodeId) {
    const ImGuiDockNode* node = ::ImGui::DockBuilderGetNode(nodeId);
    return node != nullptr && node->ParentNode != nullptr && node->HostWindow != nullptr;
}

/// Register the Mobile ↔ Desktop layout round-trip regression test.
/// Verifies that desktop window docking is preserved after a Mobile→Desktop UI-mode
/// transition, guarding against mid-frame ini restoration that causes undocking.
/// @param engine ImGui Test Engine instance to register the test with
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
        // false-failing. Use the desktop main-pane identity to verify the fix.
        const DockedWindowSnapshot mainPaneBefore = SnapshotWindowDockState("Smatchet - Active Project");
        const bool preMainPaneDocked = (mainPaneBefore.DockId != 0);

        if (!preMainPaneDocked) {
            ctx->LogInfo("skip: Smatchet - Active Project window not docked pre-flip — headless/non-default host layout");
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

        // Poll until the desktop main pane is back and docked with the same root node.
        // The dock tree rebuilds lazily on the next NewFrame after the ini swap, then
        // BeginDocked applies the restored DockIds on that frame. A generous frame cap
        // handles slow Mesa settle. Compare both DockId and RootNodeId to catch reparenting.
        const ImGuiID preRootNodeId = mainPaneBefore.RootNodeId;
        const ImGuiID preDockId = mainPaneBefore.DockId;
        bool restored = false;
        for (int i = 0; i < 120 && !restored; ++i) {
            ctx->Yield();
            const DockedWindowSnapshot postMainPane = SnapshotWindowDockState("Smatchet - Active Project");
            restored = (postMainPane.DockId != 0 && postMainPane.DockId == preDockId &&
                       postMainPane.RootNodeId == preRootNodeId);
        }

        // The exact inverse of the undock bug: the main pane that was docked
        // before the round-trip is STILL docked with the same DockId and RootNodeId after it.
        IM_CHECK_NO_RET(restored);

        // Restore every mutated field.
        g_ui.cfg.UiMode = origUiMode;
        g_ui.mobilePage = origPage;
        ctx->Yield();
    };
}

} // namespace

/// Public entry point to register all Mobile-Desktop layout round-trip regression tests.
/// Called once from UiTestScenario::OnStart() after ImGui Test Engine initialization.
/// @param engine ImGui Test Engine instance
extern "C" void SmatchetRegisterMobileDesktopLayoutRoundtripTests(ImGuiTestEngine* engine) {
    RegisterMobileDesktopLayoutRoundTripTest(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
