#include "SmatchetAutocompleteUi.h"

#include "SmatchetAutocompleteUi_detail.h"

#include "Interfaces/IAppUsers.h"
#include "JqlSuggestEngine.h"
#include "PlaneQuerySuggestEngine.h"
// SMATCHET_DEVIATION(rule=duplication; reason=pre-existing UI-TU include-block boilerplate clone re-surfaced by adding one include here; de-duping independent UI subsystems' include lists is DRY-CRITICAL; owner=tracker-backend; revisit=2026-11-30)
#include "Tracker/JqlSuggestEnginePure.h"
#include "Tracker/JqlUserDisplayPure.h"
#include "Tracker/TrackerQuerySuggestCommon.h"
#include "Tracker/TrackerBackendKind.h"
#include "SmatchetUiSession.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <unordered_set>

namespace {

/// Keep the accountId -> display-name pairing a completed user search taught us, so the
/// in-place name rewrite of the query buffer can still name the account after the popup
/// closes. De-duplicated by accountId and capped — this outlives a single search by design.
static void RememberResolvedUser(JqlEditorState& st, const TrackerUser& u) {
    constexpr size_t kMaxResolved = 200;
    for (auto& known : st.jqlAcpSearchResolvedUsers) {
        if (known.AccountId != u.AccountId) {
            continue;
        }
        // A later search can carry a corrected name for an account already retained. Take it
        // and drop the rewrite memo by hand: the memo keys on the retained COUNT, which a
        // rename leaves untouched, so without this the buffer keeps the stale name.
        if (!u.DisplayName.empty() && known.DisplayName != u.DisplayName) {
            known = u;
            st.jqlNameRewriteValid = false;
        }
        return;
    }
    if (st.jqlAcpSearchResolvedUsers.size() >= kMaxResolved) {
        st.jqlAcpSearchResolvedUsers.erase(st.jqlAcpSearchResolvedUsers.begin());
    }
    st.jqlAcpSearchResolvedUsers.push_back(u);
    // Every insertion changes what the rewrite can name, and at the cap the evict-then-append
    // pair leaves the size identical — so drop the memo here rather than infer it from a size.
    st.jqlNameRewriteValid = false;
}

/// Turn a completed user search into popup rows: one row per named account, capped, and
/// each account's name remembered for the buffer rewrite. Rows insert the same name-form
/// token the catalog path builds (BuildJqlUserInsert) so async and catalog rows dedup
/// against each other and a pick shows the user's name in the input, not the account id.
static void ReduceUserSearchResultIntoItems(JqlEditorState& st, const std::vector<TrackerUser>& users) {
    constexpr int kMaxAsyncRows = 40;
    st.jqlAcpAsyncUserItems.clear();
    std::unordered_set<std::string> seen;
    for (const auto& u : users) {
        if (u.AccountId.empty()) {
            continue;
        }
        std::string ins = BuildJqlUserInsert(u);
        if (ins.empty() || !seen.insert(ins).second) {
            continue;
        }
        RememberResolvedUser(st, u);
        std::string label = u.DisplayName.empty() ? u.AccountId : u.DisplayName;
        if (!u.DisplayName.empty() && !u.EmailAddress.empty()) {
            label += " (" + u.EmailAddress + ")";
        }
        st.jqlAcpAsyncUserItems.push_back(QuerySuggestion{std::move(label), std::move(ins)});
        if (static_cast<int>(st.jqlAcpAsyncUserItems.size()) >= kMaxAsyncRows) {
            break;
        }
    }
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
    // A user row inserts the opaque accountId while its LABEL carries the display name —
    // the only moment both are in hand. Remember the pairing here so the readable echo can
    // name the account the instant the insert lands, with no catalog and no extra fetch.
    const std::string insertedId = jql_user_display::UnquoteValueToken(st.jqlAcpReplaceText);
    if (jql_user_display::LooksLikeAccountId(insertedId)) {
        std::string name = b.Items[static_cast<size_t>(index)].Label;
        // Async search rows label as `Name -> "id"`; keep just the name half.
        const std::string arrow = " -> " + b.Items[static_cast<size_t>(index)].Insert;
        if (name.size() > arrow.size() && name.compare(name.size() - arrow.size(), arrow.size(), arrow) == 0) {
            name.erase(name.size() - arrow.size());
        }
        if (!name.empty() && name != insertedId) {
            TrackerUser u;
            u.AccountId = insertedId;
            u.DisplayName = std::move(name);
            RememberResolvedUser(st, u);
        }
    }
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
    const bool waitingUser = !smatchet::tracker::IsPlaneBackendType(d.cfg.TrackerType) &&
                             st.jqlAcpUserSearchFireAt > 0.0 && ImGui::GetTime() < st.jqlAcpUserSearchFireAt;
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

void TrackerQueryAcp_ApplyUserNamesToBuffer(const std::vector<TrackerField>& fields,
                                            const std::vector<TrackerUser>& catalogUsers, JqlEditorState& st) {
    // Memoised on the buffer text plus the catalog SNAPSHOT the names came from — its buffer
    // address and size, not a name-by-name compare, so a steady frame stays O(1) (Pillar 1).
    // The catalog is only ever replaced wholesale (`AvailableUsers = std::move(...)`, or a
    // whole-vector copy on pane switch), so a refresh lands on a new allocation and is caught
    // even when it renames a user without changing the count. The search-resolved side needs
    // no key at all: RememberResolvedUser drops the memo on every insert and rename. The
    // field catalog gates WHICH value positions may rewrite, so its snapshot is keyed the
    // same way (it too is only ever replaced wholesale).
    const void* catalogData = static_cast<const void*>(catalogUsers.data());
    const void* fieldsData = static_cast<const void*>(fields.data());
    if (st.jqlNameRewriteValid && st.jqlNameRewriteCatalogData == catalogData &&
        st.jqlNameRewriteCatalogSize == catalogUsers.size() && st.jqlNameRewriteFieldsData == fieldsData &&
        st.jqlNameRewriteFieldsSize == fields.size() && st.jqlNameRewriteSource == st.buf) {
        return;
    }
    // Catalog + search-resolved merged into ONE vector, mirroring the name→id inverse in
    // TrackerQueryAcp_QueryWithAccountIds: the pure layer's round-trip guard judges name
    // uniqueness across the list it is given, so a display name duplicated across the two
    // lists must be visible in a single pass — two passes would each see it as unique and
    // rewrite an id the inverse then refuses to undo. The merge runs on memo misses only,
    // never the steady frame (Pillar 1).
    std::vector<TrackerUser> merged = catalogUsers;
    merged.insert(merged.end(), st.jqlAcpSearchResolvedUsers.begin(), st.jqlAcpSearchResolvedUsers.end());
    int replaced = 0;
    std::string rendered = jql_user_display::RenderQueryWithUserNames(st.buf, fields, merged, &replaced);
    if (replaced > 0 && rendered.size() < sizeof(st.buf) && rendered != st.buf) {
        SmatchetViewsDashboardUiDetail::CopyStringToBuffer(st.buf, rendered);
        // Cosmetic, not an edit: the views dirty-compare consumes this flag so the rewrite
        // alone never marks the view dirty.
        st.jqlBufSemanticRewrite = true;
    }
    st.jqlNameRewriteSource = st.buf; // post-rewrite content, so the steady frame is O(1)
    st.jqlNameRewriteCatalogData = catalogData;
    st.jqlNameRewriteCatalogSize = catalogUsers.size();
    st.jqlNameRewriteFieldsData = fieldsData;
    st.jqlNameRewriteFieldsSize = fields.size();
    st.jqlNameRewriteValid = true;
}

std::string TrackerQueryAcp_QueryWithAccountIds(const std::vector<TrackerField>& fields,
                                                const std::vector<TrackerUser>& catalogUsers, const JqlEditorState& st,
                                                const std::string& query) {
    // Catalog + search-resolved merged into ONE vector: whether a display name is unique
    // must be judged across everything known at once — two separate passes would each call
    // a duplicate name split across the lists unique and silently pick the wrong account.
    // Runs on user actions only (apply / Enter / open-in-browser), never per frame, so the
    // bounded catalog copy is off the hot path (Pillar 1).
    std::vector<TrackerUser> merged = catalogUsers;
    merged.insert(merged.end(), st.jqlAcpSearchResolvedUsers.begin(), st.jqlAcpSearchResolvedUsers.end());
    return jql_user_display::RenderQueryWithAccountIds(query, fields, merged, nullptr);
}

std::string TrackerQueryAcp_CanonicalQueryForApply(const std::string& trackerType,
                                                   const std::vector<TrackerField>& fields,
                                                   const std::vector<TrackerUser>& catalogUsers,
                                                   const JqlEditorState& st, const std::string& query) {
    // Only Jira carries the id<->name rewrite (JQL matches a user by accountId); every other
    // backend's query text is already canonical.
    if (smatchet::tracker::BackendIndexFromType(trackerType) != smatchet::tracker::kBackendJira) {
        return query;
    }
    return TrackerQueryAcp_QueryWithAccountIds(fields, catalogUsers, st, query);
}

void TrackerQueryAcp_TickAccountIdResolve(const IAppUsers& userSearch, const std::vector<TrackerUser>& catalogUsers,
                                          JqlEditorState& st) {
    // Consume a completed lookup first. On success, ids the backend did not return stay
    // attempted — an UNKNOWN id costs one call per session. On FAILURE the whole batch is
    // un-attempted and retried with backoff: the first tick races backend init at startup,
    // and a permanent skip there would leave a restored view's ids unnamed all session
    // (Bugbot, #2149). Bounded so a broken config stops costing HTTP calls.
    constexpr int kJqlIdResolveMaxFailures = 5;
    if (st.jqlIdResolveFuture.valid() &&
        st.jqlIdResolveFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        JqlEditorState::JqlUserSearchResult result = st.jqlIdResolveFuture.get();
        st.jqlIdResolveInFlight = false;
        if (result.Ok) {
            st.jqlIdResolveFailures = 0;
            for (const auto& u : result.Users) {
                if (!u.AccountId.empty()) {
                    RememberResolvedUser(st, u);
                }
            }
        } else {
            ++st.jqlIdResolveFailures;
            if (st.jqlIdResolveFailures <= kJqlIdResolveMaxFailures) {
                auto& attempted = st.jqlIdResolveAttempted;
                for (const auto& id : st.jqlIdResolveInFlightIds) {
                    attempted.erase(std::remove(attempted.begin(), attempted.end(), id), attempted.end());
                }
                // 2s, 4s, ... capped at 60s; invalidate the scan memo so the retry fires
                // even when the buffer has not changed since.
                const double delay = (std::min)(60.0, std::pow(2.0, st.jqlIdResolveFailures));
                st.jqlIdResolveRetryAt = ImGui::GetTime() + delay;
                st.jqlIdResolveScanValid = false;
            }
        }
        st.jqlIdResolveInFlightIds.clear();
    }
    if (st.jqlIdResolveInFlight) {
        return;
    }
    if (st.jqlIdResolveRetryAt > 0.0 && ImGui::GetTime() < st.jqlIdResolveRetryAt) {
        return;
    }
    // Per-frame gate: rescan only when the buffer changed (one string compare otherwise),
    // mirroring the echo memo's discipline (Pillar 1).
    if (st.jqlIdResolveScanValid && st.jqlIdResolveScanSource == st.buf) {
        return;
    }
    st.jqlIdResolveScanSource = st.buf;
    st.jqlIdResolveScanValid = true;

    std::vector<std::string> pending = jql_user_display::CollectUnresolvedAccountIds(st.buf, catalogUsers);
    if (pending.empty()) {
        return;
    }
    auto knownAlready = [&](const std::string& id) {
        for (const auto& u : st.jqlAcpSearchResolvedUsers) {
            if (u.AccountId == id && !u.DisplayName.empty()) {
                return true;
            }
        }
        for (const auto& tried : st.jqlIdResolveAttempted) {
            if (tried == id) {
                return true;
            }
        }
        return false;
    };
    pending.erase(std::remove_if(pending.begin(), pending.end(), knownAlready), pending.end());
    if (pending.empty()) {
        return;
    }
    st.jqlIdResolveAttempted.insert(st.jqlIdResolveAttempted.end(), pending.begin(), pending.end());
    st.jqlIdResolveInFlightIds = pending;
    st.jqlIdResolveInFlight = true;
    st.jqlIdResolveFuture = std::async(std::launch::async, [&userSearch, pending]() {
        JqlEditorState::JqlUserSearchResult r;
        UnpackResult(userSearch.FetchUsersByAccountIds(pending), r.Ok, r.Users, r.Error);
        return r;
    });
}

void TrackerQueryAcp_TickDebouncedUserSearch(const IAppUsers& userSearch, UiDrawSession& d, JqlEditorState& st,
                                             const QuerySuggestMeta& meta, const QuerySuggestBuild& syncBuild) {
    (void)syncBuild;
    if (smatchet::tracker::IsPlaneBackendType(d.cfg.TrackerType) || !meta.UserValueToken) {
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
                ReduceUserSearchResultIntoItems(st, result.Users);
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
