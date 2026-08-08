// dock_slot_liveness.test.cpp — bucket-E regression guard for the dead-dock-slot fix.
//
// Bug (fixed): SetNextWindowDockID(id, ImGuiCond_Always) does NOT fail when `id`
// names a node that no longer exists. DockContextBindNodeToWindow calls
// DockContextAddNode and takes AuthorityForPos/Size from the window, minting an
// ORPHAN ROOT node (ParentNode == null) at the window's current rect. The window
// then LOOKS docked while owning no slot in the dockspace, and the illusion only
// breaks when the user drags a splitter and it pops out as a floating rect.
//
// Fix: SmatchetDockNodeIds::EnsureDockSlotAlive returns 0 for a dead slot so the
// caller skips the write, leaving the window visibly floating (therefore fixable)
// instead of invisibly fake-docked.
//
// The guard rejects on TWO conditions and both are load-bearing:
//   1. DockBuilderGetNode misses — a plain map lookup that creates nothing, which
//      is exactly the asymmetry against SetNextWindowDockID the guard exploits.
//   2. The node exists but ParentNode is null. An orphan minted by the PRE-FIX
//      build is persisted to imgui.ini under [Docking][Data] and reloaded on the
//      next launch, so a bare existence check would pass for precisely the ids
//      this guard exists to reject — the whole installed base. Every constant in
//      SmatchetDockNodeIds names a node the default layout cuts as a CHILD of the
//      dockspace root, so a null parent means the id resolved to a detached node.
//
// Both variants are deterministic and build their own throwaway dock nodes under
// synthetic ids, so nothing here depends on the live layout, on a realized
// HostWindow, or on a drag landing — it survives Mesa-headless GL unchanged.
// Condition 2 is the one that matters: an orphan root IS a dock node, so the
// pre-fix behaviour passes any naive existence/IsDocked check.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetDockNodeIds.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiDockNode::ParentNode, DockBuilder*, NextWindowData
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

// Synthetic ids, deliberately far from every SmatchetDockNodeIds constant so the
// test can never disturb (or be disturbed by) the running session's real layout.
const ImGuiID kProbeRoot = 0x5A17C0DEu;
const ImGuiID kProbeNever = 0x5A17DEADu;

// Minimal GuiFunc — the engine wants a window to drive; every assertion below is
// a pure dock-context state check in TestFunc.
void ProbeGui(ImGuiTestContext*) {
    ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::DockSlotLiveness", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("dock slot liveness probe");
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Variant 1: EnsureDockSlotAlive rejects 0, a never-created id, and an ORPHAN
// root; accepts a node parented inside a real tree.
// ---------------------------------------------------------------------------
void RegisterEnsureDockSlotAliveVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DockSlotLiveness", "EnsureDockSlotAlive_RejectsDeadAndOrphan");

    t->GuiFunc = ProbeGui;

    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield(); // one frame so there is a live context for IM_CHECK

        using SmatchetDockNodeIds::EnsureDockSlotAlive;

        // Slot 0 is the "this window has no default slot" sentinel, never a node.
        IM_CHECK(EnsureDockSlotAlive(0) == 0);

        // A never-created id: DockBuilderGetNode misses and creates nothing.
        IM_CHECK(ImGui::DockBuilderGetNode(kProbeNever) == nullptr);
        IM_CHECK(EnsureDockSlotAlive(kProbeNever) == 0);
        // The lookup itself must not have minted anything — that asymmetry against
        // SetNextWindowDockID is the entire basis of the guard.
        IM_CHECK(ImGui::DockBuilderGetNode(kProbeNever) == nullptr);

        // Build a bare root node. This is precisely the shape SetNextWindowDockID
        // leaves behind on a dead id, and the shape reloaded from a persisted
        // [Docking][Data] block on the next launch. DockBuilderAddNode removes any
        // existing node under the id first, so a leftover from a prior run is fine.
        ImGui::DockBuilderAddNode(kProbeRoot, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodeSize(kProbeRoot, ImVec2(400, 400));

        const ImGuiDockNode* root = ImGui::DockBuilderGetNode(kProbeRoot);
        IM_CHECK(root != nullptr);
        IM_CHECK(root->ParentNode == nullptr); // orphan root, by construction

        // The assertion that matters. An orphan root IS a dock node, so existence
        // alone passes; only the ParentNode clause rejects it.
        IM_CHECK(EnsureDockSlotAlive(kProbeRoot) == 0);

        // Split it: the two children are parented inside a real tree, which is what
        // every SmatchetDockNodeIds constant actually names in the default layout.
        ImGuiID childA = 0;
        ImGuiID childB = 0;
        ImGui::DockBuilderSplitNode(kProbeRoot, ImGuiDir_Down, 0.35f, &childA, &childB);
        ImGui::DockBuilderFinish(kProbeRoot);

        IM_CHECK(childA != 0);
        IM_CHECK(childB != 0);

        const ImGuiDockNode* nodeA = ImGui::DockBuilderGetNode(childA);
        IM_CHECK(nodeA != nullptr);
        IM_CHECK(nodeA->ParentNode != nullptr);

        // Live child slot: the guard passes it straight through unchanged.
        IM_CHECK(EnsureDockSlotAlive(childA) == childA);
        IM_CHECK(EnsureDockSlotAlive(childB) == childB);

        // The root is still an orphan after the split — a parent, not a child.
        IM_CHECK(EnsureDockSlotAlive(kProbeRoot) == 0);

        // Kill the tree and re-assert: the previously-live child is now dead, the
        // "user dragged the last tab out and the node collapsed" case.
        ImGui::DockBuilderRemoveNode(kProbeRoot);
        IM_CHECK(ImGui::DockBuilderGetNode(childA) == nullptr);
        IM_CHECK(EnsureDockSlotAlive(childA) == 0);
    };
}

// ---------------------------------------------------------------------------
// Variant 2: DockNextWindowOnFirstUse writes a dock id for a live slot and
// writes NOTHING for a dead one — asserted on ImGui's own next-window state
// rather than on an observable window position, so no host realization needed.
// ---------------------------------------------------------------------------
void RegisterDockNextWindowOnFirstUseVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DockSlotLiveness", "DockNextWindowOnFirstUse_SkipsDeadSlot");

    t->GuiFunc = ProbeGui;

    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield();

        ImGuiContext& g = *ImGui::GetCurrentContext();

        // The helper also no-ops while a mouse button is held OR on the frame it is
        // released (a dock write mid-drag fights the drag). Settle the mouse first so
        // this variant exercises the liveness branch and not the drag branch, and
        // assert that precondition rather than assuming the yields were enough.
        ctx->MouseUp(0);
        ctx->Yield(2);
        IM_CHECK(!ImGui::IsMouseDown(0));
        IM_CHECK(!ImGui::IsMouseReleased(0));

        // Dead slot: no dock id is staged at all.
        g.NextWindowData.ClearFlags();
        SmatchetDockNodeIds::DockNextWindowOnFirstUse(kProbeNever);
        IM_CHECK((g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasDock) == 0);

        // Live child slot: the dock id is staged.
        ImGui::DockBuilderAddNode(kProbeRoot, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodeSize(kProbeRoot, ImVec2(400, 400));
        ImGuiID childA = 0;
        ImGuiID childB = 0;
        ImGui::DockBuilderSplitNode(kProbeRoot, ImGuiDir_Down, 0.35f, &childA, &childB);
        ImGui::DockBuilderFinish(kProbeRoot);

        g.NextWindowData.ClearFlags();
        SmatchetDockNodeIds::DockNextWindowOnFirstUse(childA);
        IM_CHECK((g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasDock) != 0);
        IM_CHECK(g.NextWindowData.DockId == childA);

        // Orphan root: rejected exactly like the never-created id.
        g.NextWindowData.ClearFlags();
        SmatchetDockNodeIds::DockNextWindowOnFirstUse(kProbeRoot);
        IM_CHECK((g.NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasDock) == 0);

        // Leave no staged state or probe nodes behind for the next test.
        g.NextWindowData.ClearFlags();
        ImGui::DockBuilderRemoveNode(kProbeRoot);
    };
}

} // namespace

extern "C" void SmatchetRegisterDockSlotLivenessTests(ImGuiTestEngine* engine) {
    RegisterEnsureDockSlotAliveVariant(engine);
    RegisterDockNextWindowOnFirstUseVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
