#pragma once

// Pure (ImGui-free) helpers extracted from LuaConsolePlugin::OnDraw so they are
// bucket-A unit-testable without an ImGui context. Header-only inline functions
// keep the plugin a single translation unit while still being linkable from the
// test target. Behaviour is byte-identical to the original inline code.

#include <algorithm>
#include <string>
#include <vector>

namespace lua_console_detail {

/// Map selected grid rows to a sorted, de-duplicated list of ticket ids.
/// `rows` are the explicitly-selected row indices (any iterable int container —
/// the caller passes a `std::set<int>`); when `rectActive` is set the inclusive
/// `[rectMinRow, rectMaxRow]` range is also included. Each row indexes
/// `sortedIndices`, whose value indexes `ticketIds`. Out-of-range rows and
/// indices are skipped (matches the original bounds guards).
template <typename RowContainer>
inline std::vector<std::string> CollectSelectedTicketIds(const RowContainer& rows, bool rectActive, int rectMinRow,
                                                         int rectMaxRow, const std::vector<size_t>& sortedIndices,
                                                         const std::vector<std::string>& ticketIds) {
    std::vector<std::string> selectedIds;
    const auto addRow = [&](int r) {
        if (r >= 0 && static_cast<size_t>(r) < sortedIndices.size()) {
            const size_t originalIdx = sortedIndices[static_cast<size_t>(r)];
            if (originalIdx < ticketIds.size()) {
                selectedIds.push_back(ticketIds[originalIdx]);
            }
        }
    };
    for (int r : rows) {
        addRow(r);
    }
    if (rectActive) {
        for (int r = rectMinRow; r <= rectMaxRow; ++r) {
            addRow(r);
        }
    }
    std::sort(selectedIds.begin(), selectedIds.end());
    selectedIds.erase(std::unique(selectedIds.begin(), selectedIds.end()), selectedIds.end());
    return selectedIds;
}

/// Resolve the display index of a tracked selection (identified by its script
/// name/path) within the freshly refreshed, re-sorted script list. Returns the
/// matching index, or -1 when the tracked name is empty or no longer present in
/// the list (e.g. a background rescan dropped a file). Callers treat -1 as
/// "no valid selection" and must NOT fall through to a stale integer index --
/// doing so overwrote whichever unrelated file happened to sit at that index
/// after a re-sort (DR11 silent data loss). Tracking identity by name keeps the
/// selection anchored to the same file across insert/delete/reorder.
inline int ResolveSelectedScriptIndex(const std::vector<std::string>& scriptList, const std::string& selectedName) {
    if (selectedName.empty()) {
        return -1;
    }
    const auto it = std::find(scriptList.begin(), scriptList.end(), selectedName);
    if (it == scriptList.end()) {
        return -1;
    }
    return static_cast<int>(it - scriptList.begin());
}

/// True when the editor buffer is the buffer that was actually loaded FOR `selectedName`, and may
/// therefore be written back to it. `editorContentName` is the buffer's PROVENANCE — the name whose
/// read landed in the editor — and is empty whenever the editor holds the blank placeholder that
/// `StartLoadSelectedScriptIntoEditor` installs for an in-flight read.
/// The two can diverge with no read in flight and no refusal latched, which is #2042: a read result
/// dropped by `PollScriptLoad`'s name-mismatch bail leaves the editor blank, and a later
/// `SyncSelectionToList` auto-selects an unrelated script — so Save would truncate a real file with
/// a buffer that never loaded. This is a POSITIVE check rather than one more negative guard: the
/// refusal-reason guards each cover one known way a load can fail to land, whereas this covers
/// every way at once. Distinct from DR11 (`ResolveSelectedScriptIndex` above), which keeps the
/// SELECTION anchored to a file across a re-sort; this keeps the BUFFER anchored to the selection.
inline bool EditorBufferMatchesSelection(const std::string& editorContentName, const std::string& selectedName) {
    if (editorContentName.empty() || selectedName.empty()) {
        return false; // nothing loaded, or nothing targeted — never write
    }
    return editorContentName == selectedName;
}

/// Join action names into the wrapped display string shown under the run row.
inline std::string JoinRegisteredActionNames(const std::vector<std::string>& names) {
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) {
            joined += ", ";
        }
        joined += names[i];
    }
    return joined;
}

} // namespace lua_console_detail
