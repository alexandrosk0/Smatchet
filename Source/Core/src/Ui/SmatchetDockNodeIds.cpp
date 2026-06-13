#include "SmatchetDockNodeIds.h"

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

size_t DefaultDockLayoutKeyCount() { return sizeof(kEntries) / sizeof(kEntries[0]); }

const char* DefaultDockLayoutKeyAt(size_t i) {
    if (i >= DefaultDockLayoutKeyCount()) {
        return "";
    }
    return kEntries[i].key;
}

} // namespace SmatchetDockNodeIds
