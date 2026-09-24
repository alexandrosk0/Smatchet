// mobile_desktop_layout_roundtrip.test.cpp — bucket-E regression guard for the desktop
// layout undocking on a Mobile -> Desktop UI-mode round-trip (e.g. an Auto-mode window
// narrowed past the mobile breakpoint and widened back).
//
// Bug (fixed): SmatchetUI::Draw restored the desktop imgui.ini MID-FRAME on the
// Mobile->Desktop edge. LoadIniSettings rebuilds the dock tree immediately, but the host's
// DockSpaceOverViewport had already run that frame, so the rebuilt nodes were not
// LastFrameAlive and BeginDocked undocked every desktop window submitted later in the same
// frame (imgui.cpp ~21208); the floating layout was then autosaved. A second, smaller gap:
// the Desktop->Mobile edge dropped the in-memory desktop layout without flushing it, so a
// dock change inside ImGui's autosave window was lost.
//
// Fix: the edge frame still draws the mobile shell and swaps the ini back at end-of-frame,
// so desktop windows first submit on the next frame against a tree the host has already
// marked alive; the Desktop->Mobile edge flushes the desktop ini before detaching it.
//
// Why the check is FIRST-FRAME rather than eventual: repairTopLevelWindow re-docks a
// floating canonical window into its DEFAULT slot a frame later, which hides the bug for
// those windows (while silently losing any custom placement) and never re-docks
// non-canonical ones. So the assertion is that on the first frame each window is active
// again after the edge, it is docked into the exact node it occupied before the flip.
// Gated on pre-flip state so a host with no docked desktop windows (or no ini file
// attached) skips instead of false-failing.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow::DockId / DockNode / LastFrameActive, FindWindowByID
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <vector>

extern UiDrawSession g_ui;

namespace {

struct DockedWindowRecord {
    ImGuiID windowId = 0;
    ImGuiID dockId = 0;
    bool checked = false;
};

// True when the window was submitted on the most recently completed frame (robust to the
// test coroutine resuming either side of NewFrame's Active -> WasActive roll-over).
bool WasActiveLastFrame(const ImGuiWindow& window) { return window.LastFrameActive >= ::ImGui::GetFrameCount() - 1; }

// Every live top-level window that is docked right now. Child windows and dock-host windows
// carry no DockNode of their own, so the DockNode filter excludes them.
std::vector<DockedWindowRecord> SnapshotDockedWindows() {
    std::vector<DockedWindowRecord> out;
    const ImGuiContext& g = *::ImGui::GetCurrentContext();
    for (const ImGuiWindow* window : g.Windows) {
        if (window == nullptr || window->DockNode == nullptr || window->DockId == 0 || !WasActiveLastFrame(*window)) {
            continue;
        }
        DockedWindowRecord record;
        record.windowId = window->ID;
        record.dockId = window->DockId;
        out.push_back(record);
    }
    return out;
}

bool MobileShellIsLive() {
    const ImGuiWindow* shell = ::ImGui::FindWindowByName("##MobileShell");
    return shell != nullptr && WasActiveLastFrame(*shell);
}

void RegisterRoundTripKeepsDockingVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "MobileDesktopLayout", "RoundTrip_KeepsEveryWindowDocked");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const UiMode origUiMode = g_ui.cfg.UiMode;
        const MobilePage origPage = g_ui.mobilePage;
        const bool origDrawerOpen = g_ui.mobileDrawerOpen;

        // Pin Desktop and let the layout settle so the snapshot sees a steady desktop tree.
        g_ui.cfg.UiMode = UiMode::Desktop;
        for (int i = 0; i < 8; ++i) {
            ctx->Yield();
        }
        if (::ImGui::GetIO().IniFilename == nullptr) {
            ctx->LogInfo("skip: no desktop imgui.ini attached — nothing to round-trip");
            g_ui.cfg.UiMode = origUiMode;
            return;
        }
        std::vector<DockedWindowRecord> docked = SnapshotDockedWindows();
        if (docked.empty()) {
            ctx->LogInfo("skip: no docked desktop window pre-flip — headless/non-default host layout");
            g_ui.cfg.UiMode = origUiMode;
            return;
        }

        // Desktop -> Mobile. The Grid page exercises the shell's own MobileContentDock too.
        g_ui.cfg.UiMode = UiMode::Mobile;
        g_ui.mobilePage = MobilePage::Grid;
        g_ui.mobileDrawerOpen = false;
        bool shellLive = false;
        for (int i = 0; i < 300 && !shellLive; ++i) {
            ctx->Yield();
            shellLive = MobileShellIsLive();
        }
        IM_CHECK_NO_RET(shellLive);

        // Mobile -> Desktop: check each window on the first frame it is active again, before
        // repairTopLevelWindow could re-dock a floating one into its default slot.
        g_ui.cfg.UiMode = UiMode::Desktop;
        size_t remaining = shellLive ? docked.size() : 0;
        for (int frame = 0; frame < 120 && remaining > 0; ++frame) {
            ctx->Yield();
            for (DockedWindowRecord& record : docked) {
                if (record.checked) {
                    continue;
                }
                const ImGuiWindow* window = ::ImGui::FindWindowByID(record.windowId);
                if (window == nullptr || !WasActiveLastFrame(*window)) {
                    continue;
                }
                record.checked = true;
                --remaining;
                if (window->DockId != record.dockId) {
                    ctx->LogError("'%s' came back in dock 0x%08X, was 0x%08X before the round-trip", window->Name,
                                  window->DockId, record.dockId);
                }
                IM_CHECK_NO_RET(window->DockId == record.dockId);
            }
        }
        if (remaining > 0) {
            ctx->LogInfo("note: %d docked window(s) were not re-submitted within the frame budget",
                         static_cast<int>(remaining));
        }

        g_ui.cfg.UiMode = origUiMode;
        g_ui.mobilePage = origPage;
        g_ui.mobileDrawerOpen = origDrawerOpen;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterMobileDesktopLayoutRoundtripTests(ImGuiTestEngine* engine) {
    RegisterRoundTripKeepsDockingVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
