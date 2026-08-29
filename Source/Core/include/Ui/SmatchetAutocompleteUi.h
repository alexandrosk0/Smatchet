#pragma once

#include "QuerySuggestTypes.h"
#include "imgui.h"

#include <string>
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

/** Rewrite the query buffer for DISPLAY: every account id `st.buf` carries that a catalog /
 *  search-resolved user can name is replaced in place by that user's display name, so the
 *  input shows `assignee = "Jane Doe"` rather than the opaque id. Call only while the input
 *  is not focused and no pending replace is queued (the caller gates on both). Sets
 *  `st.jqlBufSemanticRewrite` when the buffer changed so callers' dirty-compares can ignore
 *  the rewrite; the id-canonical form is recovered at the apply boundaries by
 *  TrackerQueryAcp_QueryWithAccountIds. `catalogUsers` is the app-owned user catalog; the
 *  editor's own search-resolved names are folded in on top of it. */
void TrackerQueryAcp_ApplyUserNamesToBuffer(const std::vector<TrackerUser>& catalogUsers, JqlEditorState& st);

/** Map display names in `query` back to account ids (user-type field values only), resolving
 *  against the catalog + the editor's search-resolved users merged into ONE list (uniqueness
 *  of a name is judged across everything known at once). Returns the wire/disk-canonical
 *  query. Call at apply boundaries only (view save / Enter / open-in-browser), never per
 *  frame. */
std::string TrackerQueryAcp_QueryWithAccountIds(const std::vector<TrackerField>& fields,
                                                const std::vector<TrackerUser>& catalogUsers,
                                                const JqlEditorState& st, const std::string& query);

/** Apply-boundary convenience over TrackerQueryAcp_QueryWithAccountIds: on a Jira-family
 *  backend (`trackerType` resolves to kBackendJira) returns the wire/disk-canonical form of
 *  `query` with display names reverse-mapped to account ids; every other backend's query
 *  never carries the name rewrite and passes through byte-for-byte. */
std::string TrackerQueryAcp_CanonicalQueryForApply(const std::string& trackerType,
                                                   const std::vector<TrackerField>& fields,
                                                   const std::vector<TrackerUser>& catalogUsers,
                                                   const JqlEditorState& st, const std::string& query);

/** Resolve account ids the query carries but no catalog / prior search has named, via the
 *  backend's by-accountId lookup (worker thread, polled per frame). Results land in the
 *  editor's retained-user store, which the echo reads. Skips ids already attempted this
 *  session. `catalogUsers` is the app-owned user catalog. */
void TrackerQueryAcp_TickAccountIdResolve(const IAppUsers& userSearch, const std::vector<TrackerUser>& catalogUsers,
                                          JqlEditorState& st);

/** Debounced Jira user search on main thread; mutates st.jqlAcpAsyncUserItems / errors.
 *  userSearch is the narrow user-query facet of the app object. */
void TrackerQueryAcp_TickDebouncedUserSearch(const IAppUsers& userSearch, UiDrawSession& d, JqlEditorState& st,
                                             const QuerySuggestMeta& meta, const QuerySuggestBuild& syncBuild);
