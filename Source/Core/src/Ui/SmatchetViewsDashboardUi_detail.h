#pragma once

#include "Views.h"
#include "Tracker/TrackerBackendKind.h"
#include "TrackerFieldSchema.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

class AppController;
struct UiDrawSession;
struct JqlEditorState;

namespace SmatchetViewsDashboardUiDetail {

/// True for the six core tracker field ids that the Fields tab pins into its
/// "Basic fields" group (everything else is a system or custom field). Pure —
/// no ImGui / no session state — so it is bucket-A testable in isolation.
inline bool IsBasicFieldId(const std::string& id) {
    return id == "summary" || id == "assignee" || id == "priority" || id == "status" || id == "created" ||
           id == "updated";
}

/// Friendly label for a column-order key. "id" renders as "ID"; a "field:<id>"
/// key resolves against the catalog to "<Name> (<id>)", falling back to the bare
/// id when the field is absent; any other key passes through unchanged. Pure.
inline std::string PrettyColumnLabel(const std::string& key, const std::vector<TrackerField>& fields) {
    if (key == "id") {
        return "ID";
    }
    if (key.rfind("field:", 0) == 0) {
        const std::string fieldId = key.substr(6);
        auto it = std::find_if(fields.begin(), fields.end(), [&](const TrackerField& f) { return f.Id == fieldId; });
        if (it != fields.end()) {
            return it->Name + " (" + it->Id + ")";
        }
        return fieldId;
    }
    return key;
}

/// Stable ordering for two fields: by display Name, then by Id; a null pointer
/// sorts last. Matches the comparator the Fields tab uses to order its system /
/// custom groups. Pure.
inline bool FieldSortLess(const TrackerField* lhs, const TrackerField* rhs) {
    if (!lhs || !rhs) {
        return lhs != nullptr;
    }
    if (lhs->Name != rhs->Name) {
        return lhs->Name < rhs->Name;
    }
    return lhs->Id < rhs->Id;
}

/// Result of partitioning the available-field catalog for the Fields tab.
/// `visible` keeps catalog order (search-filtered); the three group vectors are
/// sorted via FieldSortLess for system / custom, while basic keeps catalog order.
struct CategorizedFields {
    std::vector<const TrackerField*> visible;
    std::vector<const TrackerField*> system;
    std::vector<const TrackerField*> custom;
    std::vector<const TrackerField*> basic;
};

bool ContainsCaseInsensitive(const std::string& text, const std::string& needle);

/// Partition `fields` into visible / system / custom / basic groups against the
/// case-insensitive `searchNeedle` (matched on Id or Name). System and custom
/// groups are sorted by FieldSortLess; visible + basic keep catalog order. Pure
/// (no ImGui / no session state) — bucket-A testable.
inline CategorizedFields CategorizeAvailableFields(const std::vector<TrackerField>& fields,
                                                   const std::string& searchNeedle) {
    CategorizedFields out;
    for (const auto& field : fields) {
        if (!ContainsCaseInsensitive(field.Id, searchNeedle) && !ContainsCaseInsensitive(field.Name, searchNeedle)) {
            continue;
        }
        out.visible.push_back(&field);
        if (field.IsCustom) {
            out.custom.push_back(&field);
        } else if (IsBasicFieldId(field.Id)) {
            out.basic.push_back(&field);
        } else {
            out.system.push_back(&field);
        }
    }
    std::sort(out.system.begin(), out.system.end(), FieldSortLess);
    std::sort(out.custom.begin(), out.custom.end(), FieldSortLess);
    return out;
}

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

/// Truncating copy into a fixed char buffer. Returns true when the source fit;
/// false when it was clipped at N-1 bytes (mid-token loss). Callers that copy a
/// CSV selection MUST treat a false return as data loss — the selected-field set
/// is the source of truth (#views-field-uncheck), the buffer is display-only.
template <size_t N> inline bool CopyStringToBuffer(char (&dst)[N], const std::string& str) {
    static_assert(N > 0, "CopyStringToBuffer requires a non-empty char array");
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(str.size(), cap);
    if (n > 0)
        std::memcpy(dst, str.data(), n);
    return str.size() <= cap;
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

/// Serialize a selected-field id set to the canonical sorted CSV form. Pure;
/// the inverse of DeserializeSelectedFields. Round-trips losslessly regardless
/// of count, so a large selection survives set -> CSV -> set with no drop
/// (#views-field-uncheck — the fixed 1024-byte buffer truncation that this fix
/// removes from the authority path). Bucket-A testable.
inline std::string SerializeSelectedFields(const std::unordered_set<std::string>& selectedFieldSet) {
    return JoinCsvLocal(ToSortedVector(selectedFieldSet));
}

/// Parse a CSV field list back into an id set. Pure; inverse of
/// SerializeSelectedFields. Bucket-A testable.
inline std::unordered_set<std::string> DeserializeSelectedFields(const std::string& csv) {
    std::unordered_set<std::string> out;
    for (const auto& id : ParseCsv(csv)) {
        out.insert(id);
    }
    return out;
}

/// Consume-once apply of a deferred view-create latch onto the views store
/// (UiDrawSession::viewsPendingCreate — see the latch docs there; Pillar 3 crash
/// fix). Creates + activates the payload view and resets the latch. Returns the
/// new active view, or null when the latch was not set. Pure with respect to
/// ImGui / session state — bucket-A testable (tests/Core/ViewsPendingCreate.test.cpp).
inline const ViewDefinition* ApplyPendingViewCreateCore(Views& viewState, bool& requested, ViewDefinition& payload) {
    if (!requested) {
        return nullptr;
    }
    requested = false;
    viewState.Create(payload);
    payload = ViewDefinition();
    return viewState.GetActiveView();
}

/// DR13b — resolve a view's display name by id from the store, for the delete-confirm prompt +
/// toast (the target may not be the active view). Returns `fallback` when the id is absent. Pure —
/// bucket-A testable.
inline std::string FindViewName(const ViewsStore& store, const std::string& id, const std::string& fallback) {
    for (const auto& view : store.Views) {
        if (view.Id == id) {
            return view.Name;
        }
    }
    return fallback;
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
/// chrome. The popup is drawn as part of this call. `drawProjectPill` appends
/// the project-scope pill beneath the bar (dashboard only — the global omnibar
/// passes false; the pill is hard-bound to the dashboard's viewJqlEditor).
/// `hint`: optional InputTextWithHint placeholder (the omnibar states what it
/// searches); null keeps the plain InputText used by the dashboard editor.
void DrawJqlQueryEditorEmbedded(AppController& app, UiDrawSession& d, JqlEditorState& st, bool drawProjectPill = true,
                                const char* hint = nullptr);

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

/// Auto-scroll the current scrollable window when a drag-drop payload is in flight and
/// the mouse hovers near the top or bottom edge. Call once per frame inside the
/// BeginChild block of any list that accepts drag-reorder.
void TickDragDropAutoScroll();

} // namespace SmatchetViewsDashboardUiDetail
