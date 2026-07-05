// reset_layout_docking.test.cpp — bucket-E regression guard for the live
// "reset layout" docking-corruption fix.
//
// Bug (fixed): SmatchetUI_ResetLayoutToDefault used to do the heavy dock work
// (ImGui::LoadIniSettingsFromMemory + force-redock arming) INLINE. It is always
// invoked mid-frame — from the menu bar (SmatchetUI_MainMenu.cpp) or a command
// handler (BuiltinCommands_Debug.cpp). A mid-frame LoadIniSettingsFromMemory
// only QUEUES the dock-node-tree rebuild for the next NewFrame, but the
// same-frame force-redock then ran against the stale/half-loaded tree and
// orphaned the fixed nodes 0x02 / 0x04 / 0x08 (ParentNode==null, HostWindow==
// null) — every window looked placed but was actually un-docked.
//
// Fix: SmatchetUI_ResetLayoutToDefault now ONLY latches
// (UiDrawSession::pendingLayoutReset). drawEndOfFramePersistence drains the
// latch via SmatchetUI_ApplyDeferredLayoutReset at end-of-frame — the timing
// that lets the queued rebuild land cleanly on the next NewFrame and the
// force-redock settle on a freshly-built tree.
//
// Two variants:
//   1. DeferralContract_LatchesOnly (PRIMARY, deterministic) — calls the real
//      SmatchetUI_ResetLayoutToDefault on a throwaway heap UiDrawSession and
//      asserts it LATCHED but did NOT do the heavy work inline. Fails the
//      instant anyone re-inlines the dock work into the mid-frame entry point
//      (the exact shape of the original bug). No host-window dependency — this
//      is the robust CI gate that survives Mesa-headless GL.
//   2. LiveHostProbe_TreeRebuildsNoOrphans (SECONDARY, informational) — drives
//      the LIVE host: triggers a reset on g_ui, Yields for the end-of-frame
//      drain + force-redock to settle, then asserts the canonical nodes stay
//      non-orphan. Gated on each node's PRE-reset realization so Mesa-headless
//      (no HostWindow even when healthy) skips cleanly instead of false-failing;
//      only a real-GL host that actually realized a node before the reset
//      asserts it survives — the precise inverse of the orphan bug.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetDockNodeIds.h"
#include "SmatchetUI.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiDockNode::ParentNode / HostWindow, DockBuilderGetNode
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <memory>

#include <nlohmann/json.hpp> // heap UiDrawSession (below) needs complete json for its vector<json> members

extern UiDrawSession g_ui;

namespace {

// ---------------------------------------------------------------------------
// Variant 1 (PRIMARY, deterministic): the mid-frame entry point only latches.
// ---------------------------------------------------------------------------
void RegisterDeferralContractVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "ResetLayout", "DeferralContract_LatchesOnly");

    // Minimal GuiFunc — the engine expects a window to drive; the assertions
    // themselves are pure state checks in TestFunc.
    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::ResetLayoutDeferral", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("deferral contract probe");
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield(); // one frame so the engine has a live context for IM_CHECK

        // Throwaway heap instance — never touch the live g_ui from the
        // deterministic variant. Default-constructs cleanly; its dtor
        // (DrainAuditReloadFuture) early-returns on the invalid future.
        std::unique_ptr<UiDrawSession> session(new UiDrawSession());
        UiDrawSession& d = *session;

        // Baseline: latch clear, heavy-work signals clear.
        d.pendingLayoutReset = false;
        d.layoutForceDefaultsFrames = 0;
        d.pendingReDockWindows.clear();

        SmatchetUI_ResetLayoutToDefault(d);

        // Contract: the mid-frame call LATCHED and did NOTHING heavy. If anyone
        // re-inlines LoadIniSettingsFromMemory + force-redock arming here, the
        // force-redock counter goes to 8 and pendingReDockWindows fills — these
        // two assertions then fail, catching the original bug's reintroduction.
        IM_CHECK(d.pendingLayoutReset == true);
        IM_CHECK(d.layoutForceDefaultsFrames == 0);
        IM_CHECK(d.pendingReDockWindows.empty());
    };
}

// ---------------------------------------------------------------------------
// Variant 2 (SECONDARY, informational): live host rebuilds a non-orphan tree.
// ---------------------------------------------------------------------------
bool NodeIsDockedNonOrphan(ImGuiID nodeId) {
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(nodeId);
    return node != nullptr && node->ParentNode != nullptr && node->HostWindow != nullptr;
}

void RegisterLiveHostProbeVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "ResetLayout", "LiveHostProbe_TreeRebuildsNoOrphans");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        using namespace SmatchetDockNodeIds;

        // Let the boot layout settle, then snapshot which canonical nodes the
        // host actually REALIZED (parented + host-window). In Mesa-headless none
        // are realized even when healthy — that's the skip path, not a failure.
        for (int i = 0; i < 8; ++i) {
            ctx->Yield();
        }
        const bool preCentral = NodeIsDockedNonOrphan(kCentralNode); // 0x02 Issues
        const bool preViews = NodeIsDockedNonOrphan(kViewsColumn);   // 0x08 Views
        const bool preSide = NodeIsDockedNonOrphan(kPrimarySideBar); // 0x04 Assistant

        if (!preCentral && !preViews && !preSide) {
            ctx->LogInfo("skip: no canonical node realized pre-reset — headless/non-default host layout");
            return;
        }

        // Snapshot the live in-memory layout so we can restore it after — a dev
        // running the full bucket-E suite against their real profile must not
        // have their working dock layout wiped by this probe. (The on-disk
        // default-ini write inside the reset matches the live-probe convention —
        // other live probes persist config too; bucket-E expects an ephemeral
        // or CI profile.)
        const char* savedIniRaw = ImGui::SaveIniSettingsToMemory(nullptr);
        const std::string savedIni = savedIniRaw != nullptr ? savedIniRaw : "";

        // Trigger a LIVE reset exactly as the menu / command path does: latch
        // only. The running Draw loop's drawEndOfFramePersistence drains it at
        // end-of-frame, the queued rebuild lands next NewFrame, then the
        // force-redock runs for layoutForceDefaultsFrames (8) frames.
        SmatchetUI_ResetLayoutToDefault(g_ui);

        // Poll-until-settled rather than a fixed frame count: the orphan bug
        // NEVER recovers (the node stays parent==null / host==null forever), so
        // a generous cap cleanly separates "slow Mesa settle" (passes late) from
        // "permanently orphaned" (the bug — fails). Eliminates the frame-count
        // flake that is exactly the bucket-E gate-escape risk.
        bool restored = false;
        for (int i = 0; i < 120 && !restored; ++i) {
            ctx->Yield();
            restored = (!preCentral || NodeIsDockedNonOrphan(kCentralNode)) &&
                       (!preViews || NodeIsDockedNonOrphan(kViewsColumn)) &&
                       (!preSide || NodeIsDockedNonOrphan(kPrimarySideBar));
        }

        // The exact inverse of the orphan bug: every node the host had realized
        // before the reset is STILL parented + hosted after it.
        IM_CHECK(restored);

        // Best-effort restore of the dev's in-session layout (deferred load +
        // a few frames to apply). The reset already persisted the default ini to
        // disk; restoring memory keeps the running session visually intact.
        if (!savedIni.empty()) {
            ImGui::LoadIniSettingsFromMemory(savedIni.c_str(), savedIni.size());
            for (int i = 0; i < 8; ++i) {
                ctx->Yield();
            }
        }
    };
}

} // namespace

extern "C" void SmatchetRegisterResetLayoutDockingTests(ImGuiTestEngine* engine) {
    RegisterDeferralContractVariant(engine);
    RegisterLiveHostProbeVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
