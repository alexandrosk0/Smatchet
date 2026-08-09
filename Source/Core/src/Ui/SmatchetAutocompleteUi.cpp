#include "SmatchetAutocompleteUi.h"

#include "SmatchetAutocompleteUi_detail.h"

#include "Interfaces/IAppUsers.h"
#include "JqlSuggestEngine.h"
#include "PlaneQuerySuggestEngine.h"
#include "Tracker/TrackerBackendKind.h"
#include "Tracker/TrackerQuerySuggestCommon.h"
#include "SmatchetUiSession.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <unordered_set>

namespace {

/// Plane exposes no user-search endpoint, so both gates below key on "is this Plane?".
/// Routed through the shared rule rather than comparing the raw string: `TrackerType` is
/// hand-editable in smatchet_config.json, and an exact match let "plane" take the non-Plane
/// branch and fire a user search the backend cannot serve. Same class as the Preferences
/// backend-index defect fixed in #1989.
bool IsPlaneBackend(const std::string& trackerType) {
    return smatchet::tracker::BackendIndexFromType(trackerType) == smatchet::tracker::kBackendPlane;
}

static void MergeAsyncUserSuggestionsIntoBuild(const JqlEditorState& st, QuerySuggestBuild& b) {
    std::unordered_set<std::string> seen;
    for (const auto& s : b.Items) {
        seen.insert(s.Insert);
    }
    for (const auto& a : st.jqlAcpAsyncUserItems) {
        if (seen.insert(a.Insert).second) {
            b.Items.push_back(a);
        }
    }
    constexpr int kMaxMerged = 120;
    if (static_cast<int>(b.Items.size()) > kMaxMerged) {
        b.Items.resize(static_cast<size_t>(kMaxMerged));
    }
}

static void RunSuggestBuild(TrackerQuerySuggestKind kind, const char* buf, int bufLen, int cursor, int selStart,
                            int selEnd, const std::vector<TrackerField>& fields, const std::vector<TrackerUser>& users,
                            QuerySuggestBuild& out, QuerySuggestMeta* meta) {
    if (kind == TrackerQuerySuggestKind::JiraJql) {
        BuildJqlSuggestions(buf, bufLen, cursor, selStart, selEnd, fields, users, out, meta);
    } else {
        BuildPlaneQuerySuggestions(buf, bufLen, cursor, selStart, selEnd, fields, out, meta);
    }
}

/**
 * Apply a pending replacement INSIDE the InputText callback using ImGui's own
 * DeleteChars/InsertChars API. This is the canonical method that keeps ImGui's
 * internal horizontal scroll, undo stack, and cursor management intact.
 * Returns the new cursor position (end of inserted text).
 */
static int ApplyInlineReplace(ImGuiInputTextCallbackData* data, int replaceStart, int replaceEnd,
                              const std::string& ins, int caretOffset = -1) {
    replaceStart = (std::max)(0, (std::min)(replaceStart, data->BufTextLen));
    replaceEnd = (std::max)(replaceStart, (std::min)(replaceEnd, data->BufTextLen));
    const int deleteLen = replaceEnd - replaceStart;
    if (deleteLen > 0) {
        data->DeleteChars(replaceStart, deleteLen);
    }
    if (!ins.empty()) {
        data->InsertChars(replaceStart, ins.c_str(), ins.c_str() + ins.size());
    }
    const int endCursor = replaceStart + static_cast<int>(ins.size());
    int newCursor = endCursor;
    if (caretOffset >= 0 && caretOffset <= static_cast<int>(ins.size())) {
        newCursor = replaceStart + caretOffset;
    }
    data->CursorPos = newCursor;
    data->SelectionStart = data->SelectionEnd = newCursor;
    return newCursor;
}

} // namespace

bool TrackerQueryAcp_QueueApplyReplacement(JqlEditorState& st, const QuerySuggestBuild& b, int index,
                                           bool /*fromMouse*/) {
    if (index < 0 || index >= static_cast<int>(b.Items.size())) {
        return false;
    }
    st.jqlAcpReplaceStart = b.ReplaceStart;
    st.jqlAcpReplaceEnd = b.ReplaceEnd;
    st.jqlAcpReplaceText = b.Items[static_cast<size_t>(index)].Insert;
    st.jqlAcpReplaceCaretOffset = SmatchetAutocompleteDetail::StripCaretAnchorSentinel(st.jqlAcpReplaceText);
    st.jqlAcpApplyReplace = true;
    st.jqlAcpListDismissed = false;
    // Always re-focus: both keyboard (Enter may deactivate InputText) and mouse (popup click
    // steals focus) need SetKeyboardFocusHere to ensure CallbackAlways fires next frame.
    st.jqlAcpWantsJqlInputFocus = true;
    return true;
}

/** No-op: replacement now applied inside callback. Kept for callers that flush after the InputText call. */
void TrackerQueryAcp_FlushPendingReplace(JqlEditorState& /*st*/) {}

// CallbackHistory event for Up and Down arrows through the suggestion list. Rebuilds suggestions,
// merges async user results, then resolves the new selection. Split out of the input callback.
static void HandleAcpHistoryEvent(ImGuiInputTextCallbackData* data, TrackerQueryAcpCallbackUserData* ud,
                                  JqlEditorState* st) {
    if (ud != nullptr && ud->fields != nullptr && ud->users != nullptr && ud->suggestBuild != nullptr) {
        RunSuggestBuild(ud->kind, data->Buf, data->BufTextLen, data->CursorPos, data->SelectionStart,
                        data->SelectionEnd, *ud->fields, *ud->users, *ud->suggestBuild, ud->meta);
        if (st != nullptr) {
            MergeAsyncUserSuggestionsIntoBuild(*st, *ud->suggestBuild);
            st->jqlAcpLastCursor = data->CursorPos;
            st->jqlAcpLastSelectionStart = data->SelectionStart;
            st->jqlAcpLastSelectionEnd = data->SelectionEnd;
            st->jqlAcpListDismissed = false;
        }
        const int n = static_cast<int>(ud->suggestBuild->Items.size());
        if (st != nullptr) {
            const bool isDown = data->EventKey == ImGuiKey_DownArrow;
            const bool isUp = data->EventKey == ImGuiKey_UpArrow;
            const SmatchetAutocompleteDetail::AcpHistoryNav nav =
                SmatchetAutocompleteDetail::ResolveAcpHistoryNav(n, st->jqlAcpListSelected, isDown, isUp);
            st->jqlAcpListSelected = nav.selected;
            if (nav.scrollToSelected) {
                st->jqlAcpScrollToSelected = true;
            }
        }
    }
}

// CallbackAlways event that fires every frame. Applies any pending inline replacement, rebuilds
// suggestions, handles the Escape, Enter, and Tab commit keys, and the post-apply caret snap.
// Split out of the input callback.
static void HandleAcpAlwaysEvent(ImGuiInputTextCallbackData* data, TrackerQueryAcpCallbackUserData* ud,
                                 JqlEditorState* st) {
    // Apply pending replacement first (before rebuilding suggestions with new buf).
    if (st != nullptr && st->jqlAcpApplyReplace) {
        st->jqlAcpApplyReplace = false;
        st->jqlAcpListSelected = -1;
        const int caretOffset = st->jqlAcpReplaceCaretOffset;
        ApplyInlineReplace(data, st->jqlAcpReplaceStart, st->jqlAcpReplaceEnd, st->jqlAcpReplaceText, caretOffset);
        st->jqlAcpReplaceStart = -1;
        st->jqlAcpReplaceEnd = -1;
        st->jqlAcpReplaceText.clear();
        st->jqlAcpReplaceCaretOffset = -1;
        // Sync the external buf so it reflects what ImGui has internally.
        SmatchetViewsDashboardUiDetail::CopyStringToBuffer(st->buf, data->Buf);
        // Force cursor+scroll to end for a few more frames so ImGui's re-init (from
        // SetKeyboardFocusHere) does not leave the scroll at 0. Skip this when a
        // mid-insert caret was requested — the snap-to-end logic would fight the
        // caret position we just set inside the parens.
        if (caretOffset < 0) {
            st->jqlAcpCaretSnapFramesRemaining = 3;
        }
    }

    RunSuggestBuild(ud->kind, data->Buf, data->BufTextLen, data->CursorPos, data->SelectionStart, data->SelectionEnd,
                    *ud->fields, *ud->users, *ud->suggestBuild, ud->meta);
    if (st != nullptr) {
        MergeAsyncUserSuggestionsIntoBuild(*st, *ud->suggestBuild);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && st != nullptr) {
        st->jqlAcpListDismissed = true;
    }
    const int n = static_cast<int>(ud->suggestBuild->Items.size());
    if (st != nullptr) {
        const bool enterDown =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool tabDown = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
        const SmatchetAutocompleteDetail::AcpCommitDecision commit =
            SmatchetAutocompleteDetail::ResolveAcpCommit(n, st->jqlAcpListSelected, enterDown, tabDown);
        st->jqlAcpListSelected = commit.selected;
        if (commit.queueApply) {
            TrackerQueryAcp_QueueApplyReplacement(*st, *ud->suggestBuild, st->jqlAcpListSelected, false);
        }
        if (commit.wantsApplyFromEnter) {
            st->jqlWantsApplyFromEnter = true;
        }
        st->jqlAcpLastCursor = data->CursorPos;
        st->jqlAcpLastSelectionStart = data->SelectionStart;
        st->jqlAcpLastSelectionEnd = data->SelectionEnd;
    }
    // Snap cursor to end of text for N frames after an inline apply to ensure ImGui scrolls
    // to the cursor after SetKeyboardFocusHere re-initialises the widget state.
    if (st != nullptr && st->jqlAcpCaretSnapFramesRemaining > 0) {
        const int endPos = data->BufTextLen;
        data->CursorPos = endPos;
        data->SelectionStart = data->SelectionEnd = endPos;
        st->jqlAcpLastCursor = endPos;
        st->jqlAcpLastSelectionStart = endPos;
        st->jqlAcpLastSelectionEnd = endPos;
        --st->jqlAcpCaretSnapFramesRemaining;
    }
}

int TrackerQueryAcp_InputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* ud = static_cast<TrackerQueryAcpCallbackUserData*>(data->UserData);
    JqlEditorState* st = ud != nullptr ? ud->editor : nullptr;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        HandleAcpHistoryEvent(data, ud, st);
        return 0;
    }

    if (st != nullptr) {
        st->jqlAcpLastCursor = data->CursorPos;
        st->jqlAcpLastSelectionStart = data->SelectionStart;
        st->jqlAcpLastSelectionEnd = data->SelectionEnd;
    }

    if (ud != nullptr && ud->fields != nullptr && ud->users != nullptr && ud->suggestBuild != nullptr &&
        data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        HandleAcpAlwaysEvent(data, ud, st);
    }

    return 0;
}

void TrackerQueryAcp_DrawPopup(UiDrawSession& d, JqlEditorState& st, const ImVec2& fieldRectMin,
                               const ImVec2& fieldRectSize, const QuerySuggestBuild& syncBuild,
                               const std::vector<QuerySuggestion>& mergedItems) {
    const bool showList = !mergedItems.empty();
    const bool waitingUser = !IsPlaneBackend(d.cfg.TrackerType) && st.jqlAcpUserSearchFireAt > 0.0 &&
                             ImGui::GetTime() < st.jqlAcpUserSearchFireAt;
    const bool showFooterOnly = !showList && (!st.jqlAcpAsyncUserError.empty() || waitingUser);
    if (!showList && !showFooterOnly) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(fieldRectMin.x, fieldRectMin.y + fieldRectSize.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(fieldRectSize.x, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::Begin("##TrackerQueryAcpPopup", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_AlwaysAutoResize);

    constexpr float kListMaxPx = 200.0f;
    ImGui::BeginChild("##TrackerQueryAcpScroll", ImVec2(0, kListMaxPx), true);
    const int n = static_cast<int>(mergedItems.size());
    const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
    const bool mouseMoved = mouseDelta.x != 0.0f || mouseDelta.y != 0.0f;
    for (int i = 0; i < n; ++i) {
        const bool sel = (i == st.jqlAcpListSelected);
        const ImGuiSelectableFlags flags =
            ImGuiSelectableFlags_NoAutoClosePopups | (sel ? ImGuiSelectableFlags_Highlight : ImGuiSelectableFlags_None);
        const bool rowPress = ImGui::Selectable(mergedItems[static_cast<size_t>(i)].Label.c_str(), sel, flags);
        const bool reclickSelected = sel && ImGui::IsItemClicked(0);
        if (rowPress || reclickSelected) {
            st.jqlAcpReplaceStart = syncBuild.ReplaceStart;
            st.jqlAcpReplaceEnd = syncBuild.ReplaceEnd;
            st.jqlAcpReplaceText = mergedItems[static_cast<size_t>(i)].Insert;
            st.jqlAcpReplaceCaretOffset = SmatchetAutocompleteDetail::StripCaretAnchorSentinel(st.jqlAcpReplaceText);
            st.jqlAcpApplyReplace = true;
            st.jqlAcpListDismissed = false;
            st.jqlAcpWantsJqlInputFocus = true;
        }
        if (sel) {
            ImGui::SetItemDefaultFocus();
            if (st.jqlAcpScrollToSelected) {
                ImGui::SetScrollHereY(0.5f);
                st.jqlAcpScrollToSelected = false;
            }
        }
        if (mouseMoved && ImGui::IsItemHovered()) {
            st.jqlAcpListSelected = i;
        }
    }
    if (waitingUser && n == 0) {
        ImGui::TextDisabled("Searching\xe2\x80\xa6");
    }
    ImGui::EndChild();
    if (!st.jqlAcpAsyncUserError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", st.jqlAcpAsyncUserError.c_str());
        ImGui::PopStyleColor();
    } else if (!showList && !waitingUser) {
        ImGui::TextDisabled("No matches");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void TrackerQueryAcp_TickDebouncedUserSearch(const IAppUsers& userSearch, UiDrawSession& d, JqlEditorState& st,
                                             const QuerySuggestMeta& meta, const QuerySuggestBuild& syncBuild) {
    (void)syncBuild;
    if (IsPlaneBackend(d.cfg.TrackerType) || !meta.UserValueToken) {
        st.jqlAcpAsyncUserItems.clear();
        st.jqlAcpAsyncUserError.clear();
        st.jqlAcpUserSearchFireAt = 0.0;
        st.jqlAcpUserSearchQuery.clear();
        st.jqlAcpUserSearchInFlightId = 0;
        return;
    }
    const std::string& q = meta.UserSearchPrefix;
    if (q.size() < 2) {
        st.jqlAcpAsyncUserItems.clear();
        st.jqlAcpAsyncUserError.clear();
        st.jqlAcpUserSearchFireAt = 0.0;
        st.jqlAcpUserSearchQuery.clear();
        st.jqlAcpUserSearchInFlightId = 0;
        return;
    }
    if (q != st.jqlAcpUserSearchQuery) {
        ++st.jqlAcpUserSearchRequestId;
        st.jqlAcpUserSearchQuery = q;
        st.jqlAcpUserSearchArmedId = st.jqlAcpUserSearchRequestId;
        st.jqlAcpUserSearchFireAt = ImGui::GetTime() + 0.22;
        st.jqlAcpAsyncUserItems.clear();
        st.jqlAcpAsyncUserError.clear();
        return;
    }
    // Poll any pending future first — non-blocking; consume result when ready and matching id.
    if (st.jqlAcpUserSearchFuture.valid() &&
        st.jqlAcpUserSearchFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        JqlEditorState::JqlUserSearchResult result = st.jqlAcpUserSearchFuture.get();
        const uint64_t completedId = st.jqlAcpUserSearchInFlightId;
        st.jqlAcpUserSearchInFlightId = 0;
        // Drop stale results when the user typed past this request (armed id moved on).
        if (completedId == st.jqlAcpUserSearchArmedId) {
            if (!result.Ok) {
                st.jqlAcpAsyncUserError = result.Error;
                st.jqlAcpAsyncUserItems.clear();
            } else {
                st.jqlAcpAsyncUserError.clear();
                st.jqlAcpAsyncUserItems.clear();
                std::unordered_set<std::string> seen;
                for (const auto& u : result.Users) {
                    if (u.AccountId.empty()) {
                        continue;
                    }
                    const std::string ins = tracker_query_suggest::InsertForValueToken(u.AccountId);
                    if (!seen.insert(ins).second) {
                        continue;
                    }
                    std::string label = u.DisplayName.empty() ? u.AccountId : (u.DisplayName + " -> " + ins);
                    st.jqlAcpAsyncUserItems.push_back(QuerySuggestion{std::move(label), ins});
                    if (static_cast<int>(st.jqlAcpAsyncUserItems.size()) >= 40) {
                        break;
                    }
                }
            }
        }
    }
    if (st.jqlAcpUserSearchFireAt <= 0.0) {
        return;
    }
    const double now = ImGui::GetTime();
    if (now < st.jqlAcpUserSearchFireAt) {
        return;
    }
    if (st.jqlAcpUserSearchArmedId != st.jqlAcpUserSearchRequestId) {
        st.jqlAcpUserSearchFireAt = 0.0;
        return;
    }
    if (st.jqlAcpUserSearchInFlightId != 0) {
        // Already issued for this request id; let it complete (poll above next frame).
        return;
    }
    if (st.jqlAcpUserSearchFuture.valid() &&
        st.jqlAcpUserSearchFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        // A prior async user-search is still running (e.g. left in-flight after an early reset
        // of jqlAcpUserSearchInFlightId above, where the future is not consumed). Reassigning
        // jqlAcpUserSearchFuture now would run ~future on the std::async result, which BLOCKS the
        // UI thread until the worker finishes (Pillar 2 — finding #2). Defer: the poll above
        // consumes it once ready, then we re-dispatch next frame.
        return;
    }

    // Dispatch to worker thread via std::async — Jira/Plane user search HTTP must not block
    // the UI thread (Pillar 2 — finding #3). The captured facet ref aliases the app object,
    // which outlives any UI frame, so the ref capture is safe; the future is consumed on the
    // UI thread above.
    const std::string capturedQ = q;
    const uint64_t inFlightId = st.jqlAcpUserSearchArmedId;
    st.jqlAcpUserSearchInFlightId = inFlightId;
    st.jqlAcpUserSearchFireAt = 0.0;
    st.jqlAcpUserSearchFuture = std::async(std::launch::async, [&userSearch, capturedQ]() {
        JqlEditorState::JqlUserSearchResult r;
        UnpackResult(userSearch.SearchUsersByQuery(capturedQ), r.Ok, r.Users, r.Error);
        return r;
    });
}
