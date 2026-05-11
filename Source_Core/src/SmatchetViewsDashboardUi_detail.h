#pragma once

#include "Views.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

class AppController;
struct UiDrawSession;

namespace SmatchetViewsDashboardUiDetail {

inline std::string JoinCsvLocal(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0)
            out += ", ";
        out += values[i];
    }
    return out;
}

inline std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            size_t start = 0;
            size_t end = current.size();
            while (start < end && (current[start] == ' ' || current[start] == '\t'))
                ++start;
            while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t'))
                --end;
            if (end > start) {
                result.push_back(current.substr(start, end - start));
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    size_t start = 0;
    size_t end = current.size();
    while (start < end && (current[start] == ' ' || current[start] == '\t'))
        ++start;
    while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t'))
        --end;
    if (end > start) {
        result.push_back(current.substr(start, end - start));
    }
    return result;
}

template <size_t N> inline void CopyStringToBuffer(char (&dst)[N], const std::string& str) {
    static_assert(N > 0, "CopyStringToBuffer requires a non-empty char array");
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(str.size(), cap);
    if (n > 0)
        std::memcpy(dst, str.data(), n);
}

inline bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty())
        return true;
    auto toLower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string loweredText(text.size(), '\0');
    std::string loweredNeedle(needle.size(), '\0');
    std::transform(text.begin(), text.end(), loweredText.begin(), toLower);
    std::transform(needle.begin(), needle.end(), loweredNeedle.begin(), toLower);
    return loweredText.find(loweredNeedle) != std::string::npos;
}

inline std::vector<std::string> ToSortedVector(const std::unordered_set<std::string>& values) {
    std::vector<std::string> result(values.begin(), values.end());
    std::sort(result.begin(), result.end());
    return result;
}

void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory);

/// Snapshot the active view's full ViewDefinition into UiDrawSession on the
/// first transition into dirty state. Used by the unsaved-layout strip's
/// Discard button to revert widths / sort specs / column order / name / JQL /
/// fields back to disk. Cheap to call every frame — bails fast if already
/// snapshotted. The snapshot is cleared by Save / Discard / view switch.
void SnapshotActiveViewIfNeeded(UiDrawSession& d, const ViewDefinition& view);

void ApplyViewsActiveJqlFromBuffers(AppController& app, UiDrawSession& d, Views& viewState,
                                    const ViewDefinition& activeView);

/// Embedded JQL editor for the Filter tab — input + clear + autocomplete
/// popup. The surrounding tab provides its own label and "open in browser"
/// chrome. The popup is drawn as part of this call.
void DrawJqlQueryEditorEmbedded(AppController& app, UiDrawSession& d);

/// Vertical splitter that lets the user drag a horizontal divider; stores the new
/// height in *height and clamps to [min, max]. Triggers ConfigManager::Save(d.cfg)
/// when the user releases the mouse so layout survives restart.
void DrawVerticalSplitter(const char* id, UiDrawSession& d, float* height, float minHeight, float maxHeight);

/// Horizontal splitter (vertical divider line). *widthLeft is the left-pane width
/// in pixels; *widthLeft is updated on drag and persisted on release.
void DrawHorizontalSplitter(const char* id, UiDrawSession& d, float* widthLeft, float minLeft, float maxLeft);

/// Renders a drag handle glyph (vertical dots) sized to a row of frame-height,
/// changes the cursor on hover, and treats subsequent same-row item as a drag
/// source for the payload type "VIEWS_REORDER_ROW" carrying the row index.
void DrawDragHandle(const char* id, int rowIndex, const char* payloadType);

/// Begins a drag-and-drop target on the previously drawn item that accepts the
/// "VIEWS_REORDER_ROW" payload and reorders entries in `order` accordingly.
/// Also handles Alt+Up / Alt+Down for the focused row when keyboardFocusRow ==
/// rowIndex, swapping with neighbour and updating *keyboardFocusRow.
/// Returns true if the order was mutated this frame (caller can mark dirty).
bool HandleRowReorder(int rowIndex, std::vector<std::string>& order, int* keyboardFocusRow, const char* payloadType);

} // namespace SmatchetViewsDashboardUiDetail
