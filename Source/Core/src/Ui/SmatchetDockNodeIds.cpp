#include "SmatchetDockNodeIds.h"

#include <cstring>

namespace SmatchetDockNodeIds {

ImGuiID DefaultDockSlotForLayoutKey(const char* layoutKey) {
    if (!layoutKey || layoutKey[0] == '\0') {
        return 0;
    }
    struct Entry {
        const char* key;
        ImGuiID slot;
    };
    static const Entry kEntries[] = {
        {"active", kCentralNode}, {"views", kViewsColumn},       {"preferences", kBottomPanel},
        {"log", kBottomPanel},    {"performance", kBottomPanel}, {"scripting", kBottomPanel},
        {"mcp", kBottomPanel},    {"bulk_import", kBottomPanel}, {"bulk_export", kBottomPanel},
        {"audit", kBottomPanel},  {"annotate", kBottomPanel},    {"plandocs", kBottomPanel},
    };
    for (const auto& e : kEntries) {
        if (std::strcmp(layoutKey, e.key) == 0) {
            return e.slot;
        }
    }
    return 0;
}

} // namespace SmatchetDockNodeIds
