#pragma once

#include "QuerySuggestTypes.h"
#include "imgui.h"

#include <vector>

struct ImGuiInputTextCallbackData;
class IAppUsers;
struct TrackerField;
struct TrackerUser;
struct UiDrawSession;
struct JqlEditorState;

/** Which tracker-specific suggester backs the shared autocomplete shell. */
enum class TrackerQuerySuggestKind {
    JiraJql,
    PlaneFilter,
};

/** fields / users point at the app-owned catalogs (callers holding the full app object pass the
 *  catalog getters' results; the referenced storage outlives any frame). */
struct TrackerQueryAcpCallbackUserData {
    UiDrawSession* session = nullptr;
    JqlEditorState* editor = nullptr;
    const std::vector<TrackerField>* fields = nullptr;
    const std::vector<TrackerUser>* users = nullptr;
    QuerySuggestBuild* suggestBuild = nullptr;
    QuerySuggestMeta* meta = nullptr;
    TrackerQuerySuggestKind kind = TrackerQuerySuggestKind::JiraJql;
};

/** ImGui InputText callback: suggestions, arrows, Enter/Tab/Esc, caret restore. */
int TrackerQueryAcp_InputTextCallback(ImGuiInputTextCallbackData* data);

bool TrackerQueryAcp_QueueApplyReplacement(JqlEditorState& st, const QuerySuggestBuild& b, int index, bool fromMouse);

/** Apply pending replace queued by TrackerQueryAcp_QueueApplyReplacement into st.buf. */
void TrackerQueryAcp_FlushPendingReplace(JqlEditorState& st);

/**
 * Draw anchored suggestion list + footer; handles mouse hover selection.
 * mergedItems = sync + async (caller merges).
 */
/** syncBuild supplies replace span for all merged rows (async rows share same token range). */
void TrackerQueryAcp_DrawPopup(UiDrawSession& d, JqlEditorState& st, const ImVec2& fieldRectMin,
                               const ImVec2& fieldRectSize, const QuerySuggestBuild& syncBuild,
                               const std::vector<QuerySuggestion>& mergedItems);

/** Draw the readable echo of the query under the query bar: the same query with every
 *  account id it carries replaced by that user's display name. Nothing is drawn when the
 *  query names no resolvable account. `catalogUsers` is the app-owned user catalog; the
 *  editor's own search-resolved names are folded in on top of it. Presentation only — the
 *  query of record in `st.buf` (the one the backend runs) is never rewritten. */
void TrackerQueryAcp_DrawUserEcho(const std::vector<TrackerUser>& catalogUsers, JqlEditorState& st);

/** Debounced Jira user search on main thread; mutates st.jqlAcpAsyncUserItems / errors.
 *  userSearch is the narrow user-query facet of the app object. */
void TrackerQueryAcp_TickDebouncedUserSearch(const IAppUsers& userSearch, UiDrawSession& d, JqlEditorState& st,
                                             const QuerySuggestMeta& meta, const QuerySuggestBuild& syncBuild);
