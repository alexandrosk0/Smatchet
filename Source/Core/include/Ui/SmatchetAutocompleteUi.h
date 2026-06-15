#pragma once

#include "QuerySuggestTypes.h"
#include "imgui.h"

struct ImGuiInputTextCallbackData;
class AppController;
struct UiDrawSession;
struct JqlEditorState;

/** Which tracker-specific suggester backs the shared autocomplete shell. */
enum class TrackerQuerySuggestKind {
    JiraJql,
    PlaneFilter,
};

struct TrackerQueryAcpCallbackUserData {
    UiDrawSession* session = nullptr;
    JqlEditorState* editor = nullptr;
    AppController* app = nullptr;
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

/** Debounced Jira user search on main thread; mutates st.jqlAcpAsyncUserItems / errors. */
void TrackerQueryAcp_TickDebouncedUserSearch(const AppController& app, UiDrawSession& d, JqlEditorState& st,
                                             const QuerySuggestMeta& meta, const QuerySuggestBuild& syncBuild);
