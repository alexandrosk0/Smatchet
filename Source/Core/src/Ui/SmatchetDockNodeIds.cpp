#include "SmatchetDockNodeIds.h"

#include "imgui_internal.h"

#include <cstring>

namespace SmatchetDockNodeIds {
namespace {

struct Entry {
    const char* key;
    ImGuiID slot;
};

// Single source of truth: layout key -> default dock slot. Both the slot lookup and the
// key-iteration accessors below read this table, so adding a window here registers it for
// both default-slot docking and the live-reset force-redock.
const Entry kEntries[] = {
    {"active", kCentralNode},    {"views", kViewsColumn},       {"preferences", kBottomPanel},
    {"log", kBottomPanel},       {"performance", kBottomPanel}, {"scripting", kBottomPanel},
    {"mcp", kBottomPanel},       {"bulk_import", kBottomPanel}, {"bulk_export", kBottomPanel},
    {"audit", kBottomPanel},     {"annotate", kBottomPanel},    {"plandocs", kBottomPanel},
    {"user_info", kBottomPanel},
};

} // namespace

ImGuiID DefaultDockSlotForLayoutKey(const char* layoutKey) {
    if (!layoutKey || layoutKey[0] == '\0') {
        return 0;
    }
    for (const auto& e : kEntries) {
        if (std::strcmp(layoutKey, e.key) == 0) {
            return e.slot;
        }
    }
    return 0;
}

ImGuiID EnsureDockSlotAlive(ImGuiID slot) {
    if (slot == 0 || ImGui::GetCurrentContext() == nullptr) {
        return 0;
    }
    // DockBuilderGetNode is a plain lookup in the dock context — no node is created on a
    // miss, unlike SetNextWindowDockID, which is exactly the asymmetry this guard exists for.
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(slot);
    if (node == nullptr) {
        return 0;
    }
    // Existence alone is not enough. An orphan root node minted by an earlier build is persisted
    // to imgui.ini under [Docking][Data] and reloaded on the next launch, so the lookup above
    // would succeed for exactly the ids this guard exists to reject. Every constant in the header
    // names a node the default layout cuts as a CHILD of the dockspace root, so a null ParentNode
    // means the id resolved to a detached node rather than to a slot inside the dockspace.
    if (node->ParentNode == nullptr) {
        return 0;
    }
    return slot;
}

void DockNextWindowOnFirstUse(ImGuiID slot) {
    const ImGuiID live = EnsureDockSlotAlive(slot);
    if (live == 0 || ImGui::IsMouseDown(0) || ImGui::IsMouseReleased(0)) {
        return;
    }
    ImGui::SetNextWindowDockID(live, ImGuiCond_FirstUseEver);
}

size_t DefaultDockLayoutKeyCount() { return sizeof(kEntries) / sizeof(kEntries[0]); }

const char* DefaultDockLayoutKeyAt(size_t i) {
    if (i >= DefaultDockLayoutKeyCount()) {
        return "";
    }
    return kEntries[i].key;
}

} // namespace SmatchetDockNodeIds
