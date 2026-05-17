#include "SmatchetAutocompleteUi.h"

#include "AppController.h"
#include "JqlSuggestEngine.h"
#include "PlaneQuerySuggestEngine.h"
#include "SmatchetUiSession.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <unordered_set>

namespace {

static void MergeAsyncUserSuggestionsIntoBuild(const UiDrawSession& d, QuerySuggestBuild& b) {
    std::unordered_set<std::string> seen;
    for (const auto& s : b.Items) {
        seen.insert(s.Insert);
    }
    for (const auto& a : d.jqlAcpAsyncUserItems) {
        if (seen.insert(a.Insert).second) {
            b.Items.push_back(a);
        }
    }
    constexpr int kMaxMerged = 120;
    if (static_cast<int>(b.Items.size()) > kMaxMerged) {
        b.Items.resize(static_cast<size_t>(kMaxMerged));
    }
}

static bool ValueNeedsQuotesForId(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    return std::any_of(s.begin(), s.end(), [](unsigned char ch) {
        const bool ok = std::isalnum(ch) != 0 || ch == '_' || ch == '.' || ch == '-';
        return !ok || ch == '"' || ch == '\\';
    });
}

static std::string QuotedJqlStyle(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
    return out;
}

static std::string InsertTokenForUserAccountId(const std::string& accountId) {
    if (ValueNeedsQuotesForId(accountId)) {
        return QuotedJqlStyle(accountId);
    }
    return accountId;
}

static void RunSuggestBuild(TrackerQuerySuggestKind kind, const char* buf, int bufLen, int cursor, int selStart,
                            int selEnd, const AppController& app, QuerySuggestBuild& out, QuerySuggestMeta* meta) {
    if (kind == TrackerQuerySuggestKind::JiraJql) {
        BuildJqlSuggestions(buf, bufLen, cursor, selStart, selEnd, app, out, meta);
    } else {
        BuildPlaneQuerySuggestions(buf, bufLen, cursor, selStart, selEnd, app, out, meta);
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

/** Strip the \x7F caret-anchor sentinel from `text` (used by JQL function suggestions like
 *  `membersOf("\x7F")` so the caret lands between the parens on insert). Returns the byte
 *  offset where the sentinel was, or -1 if not present. */
static int StripCaretAnchorSentinel(std::string& text) {
    const std::string::size_type pos = text.find('\x7F');
    if (pos == std::string::npos) {
        return -1;
    }
    text.erase(pos, 1);
    return static_cast<int>(pos);
}

} // namespace

bool TrackerQueryAcp_QueueApplyReplacement(UiDrawSession& d, const QuerySuggestBuild& b, int index,
                                           bool /*fromMouse*/) {
    if (index < 0 || index >= static_cast<int>(b.Items.size())) {
        return false;
    }
    d.jqlAcpReplaceStart = b.ReplaceStart;
    d.jqlAcpReplaceEnd = b.ReplaceEnd;
    d.jqlAcpReplaceText = b.Items[static_cast<size_t>(index)].Insert;
    d.jqlAcpReplaceCaretOffset = StripCaretAnchorSentinel(d.jqlAcpReplaceText);
    d.jqlAcpApplyReplace = true;
    d.jqlAcpListDismissed = false;
    // Always re-focus: both keyboard (Enter may deactivate InputText) and mouse (popup click
    // steals focus) need SetKeyboardFocusHere to ensure CallbackAlways fires next frame.
    d.jqlAcpWantsJqlInputFocus = true;
    return true;
}

/** No-op: replacement now applied inside callback. Kept for callers that flush after the InputText call. */
void TrackerQueryAcp_FlushPendingReplace(UiDrawSession& /*d*/) {}

int TrackerQueryAcp_InputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* ud = static_cast<TrackerQueryAcpCallbackUserData*>(data->UserData);
    UiDrawSession* d = ud != nullptr ? ud->session : nullptr;

    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        if (ud != nullptr && ud->app != nullptr && ud->suggestBuild != nullptr) {
            RunSuggestBuild(ud->kind, data->Buf, data->BufTextLen, data->CursorPos, data->SelectionStart,
                            data->SelectionEnd, *ud->app, *ud->suggestBuild, ud->meta);
            if (d != nullptr) {
                MergeAsyncUserSuggestionsIntoBuild(*d, *ud->suggestBuild);
                d->jqlAcpLastCursor = data->CursorPos;
                d->jqlAcpLastSelectionStart = data->SelectionStart;
                d->jqlAcpLastSelectionEnd = data->SelectionEnd;
                d->jqlAcpListDismissed = false;
            }
            const int n = static_cast<int>(ud->suggestBuild->Items.size());
            if (d != nullptr) {
                if (n > 0) {
                    if (data->EventKey == ImGuiKey_DownArrow) {
                        if (d->jqlAcpListSelected < 0) {
                            d->jqlAcpListSelected = 0;
                        } else {
                            d->jqlAcpListSelected = (std::min)(n - 1, d->jqlAcpListSelected + 1);
                        }
                        d->jqlAcpScrollToSelected = true;
                    } else if (data->EventKey == ImGuiKey_UpArrow) {
                        if (d->jqlAcpListSelected >= 0) {
                            d->jqlAcpListSelected = (std::max)(0, d->jqlAcpListSelected - 1);
                            d->jqlAcpScrollToSelected = true;
                        }
                    } else if (d->jqlAcpListSelected >= 0) {
                        d->jqlAcpListSelected = (std::min)(d->jqlAcpListSelected, n - 1);
                    }
                } else {
                    d->jqlAcpListSelected = -1;
                }
            }
        }
        return 0;
    }

    if (d != nullptr) {
        d->jqlAcpLastCursor = data->CursorPos;
        d->jqlAcpLastSelectionStart = data->SelectionStart;
        d->jqlAcpLastSelectionEnd = data->SelectionEnd;
    }

    if (ud != nullptr && ud->app != nullptr && ud->suggestBuild != nullptr &&
        data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {

        // Apply pending replacement first (before rebuilding suggestions with new buf).
        if (d != nullptr && d->jqlAcpApplyReplace) {
            d->jqlAcpApplyReplace = false;
            d->jqlAcpListSelected = -1;
            const int caretOffset = d->jqlAcpReplaceCaretOffset;
            ApplyInlineReplace(data, d->jqlAcpReplaceStart, d->jqlAcpReplaceEnd, d->jqlAcpReplaceText, caretOffset);
            d->jqlAcpReplaceStart = -1;
            d->jqlAcpReplaceEnd = -1;
            d->jqlAcpReplaceText.clear();
            d->jqlAcpReplaceCaretOffset = -1;
            // Sync the external buf so it reflects what ImGui has internally.
            SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d->viewJqlBuf, data->Buf);
            // Force cursor+scroll to end for a few more frames so ImGui's re-init (from
            // SetKeyboardFocusHere) does not leave the scroll at 0. Skip this when a
            // mid-insert caret was requested — the snap-to-end logic would fight the
            // caret position we just set inside the parens.
            if (caretOffset < 0) {
                d->jqlAcpCaretSnapFramesRemaining = 3;
            }
        }

        RunSuggestBuild(ud->kind, data->Buf, data->BufTextLen, data->CursorPos, data->SelectionStart,
                        data->SelectionEnd, *ud->app, *ud->suggestBuild, ud->meta);
        if (d != nullptr) {
            MergeAsyncUserSuggestionsIntoBuild(*d, *ud->suggestBuild);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && d != nullptr) {
            d->jqlAcpListDismissed = true;
        }
        const int n = static_cast<int>(ud->suggestBuild->Items.size());
        if (d != nullptr) {
            const bool enterDown =
                ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            const bool tabDown = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
            if (n > 0) {
                if (d->jqlAcpListSelected >= 0) {
                    d->jqlAcpListSelected = (std::min)(d->jqlAcpListSelected, n - 1);
                }
                if ((enterDown || tabDown) && d->jqlAcpListSelected >= 0) {
                    TrackerQueryAcp_QueueApplyReplacement(*d, *ud->suggestBuild, d->jqlAcpListSelected, false);
                } else if (enterDown) {
                    d->jqlWantsApplyFromEnter = true;
                }
            } else {
                d->jqlAcpListSelected = -1;
                if (enterDown) {
                    d->jqlWantsApplyFromEnter = true;
                }
            }
            d->jqlAcpLastCursor = data->CursorPos;
            d->jqlAcpLastSelectionStart = data->SelectionStart;
            d->jqlAcpLastSelectionEnd = data->SelectionEnd;
        }
        // Snap cursor to end of text for N frames after an inline apply to ensure ImGui scrolls
        // to the cursor after SetKeyboardFocusHere re-initialises the widget state.
        if (d != nullptr && d->jqlAcpCaretSnapFramesRemaining > 0) {
            const int endPos = data->BufTextLen;
            data->CursorPos = endPos;
            data->SelectionStart = data->SelectionEnd = endPos;
            d->jqlAcpLastCursor = endPos;
            d->jqlAcpLastSelectionStart = endPos;
            d->jqlAcpLastSelectionEnd = endPos;
            --d->jqlAcpCaretSnapFramesRemaining;
        }
    }

    return 0;
}

void TrackerQueryAcp_DrawPopup(UiDrawSession& d, const ImVec2& fieldRectMin, const ImVec2& fieldRectSize,
                               const QuerySuggestBuild& syncBuild, const std::vector<QuerySuggestion>& mergedItems) {
    const bool showList = !mergedItems.empty();
    const bool waitingUser =
        d.cfg.TrackerType != "Plane" && d.jqlAcpUserSearchFireAt > 0.0 && ImGui::GetTime() < d.jqlAcpUserSearchFireAt;
    const bool showFooterOnly = !showList && (!d.jqlAcpAsyncUserError.empty() || waitingUser);
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
        const bool sel = (i == d.jqlAcpListSelected);
        const ImGuiSelectableFlags flags =
            ImGuiSelectableFlags_NoAutoClosePopups | (sel ? ImGuiSelectableFlags_Highlight : ImGuiSelectableFlags_None);
        const bool rowPress = ImGui::Selectable(mergedItems[static_cast<size_t>(i)].Label.c_str(), sel, flags);
        const bool reclickSelected = sel && ImGui::IsItemClicked(0);
        if (rowPress || reclickSelected) {
            d.jqlAcpReplaceStart = syncBuild.ReplaceStart;
            d.jqlAcpReplaceEnd = syncBuild.ReplaceEnd;
            d.jqlAcpReplaceText = mergedItems[static_cast<size_t>(i)].Insert;
            d.jqlAcpReplaceCaretOffset = StripCaretAnchorSentinel(d.jqlAcpReplaceText);
            d.jqlAcpApplyReplace = true;
            d.jqlAcpListDismissed = false;
            d.jqlAcpWantsJqlInputFocus = true;
        }
        if (sel) {
            ImGui::SetItemDefaultFocus();
            if (d.jqlAcpScrollToSelected) {
                ImGui::SetScrollHereY(0.5f);
                d.jqlAcpScrollToSelected = false;
            }
        }
        if (mouseMoved && ImGui::IsItemHovered()) {
            d.jqlAcpListSelected = i;
        }
    }
    if (waitingUser && n == 0) {
        ImGui::TextDisabled("Searching\xe2\x80\xa6");
    }
    ImGui::EndChild();
    if (!d.jqlAcpAsyncUserError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", d.jqlAcpAsyncUserError.c_str());
        ImGui::PopStyleColor();
    } else if (!showList && !waitingUser) {
        ImGui::TextDisabled("No matches");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void TrackerQueryAcp_TickDebouncedUserSearch(const AppController& app, UiDrawSession& d, const QuerySuggestMeta& meta,
                                             const QuerySuggestBuild& syncBuild) {
    (void)syncBuild;
    if (d.cfg.TrackerType == "Plane" || !meta.UserValueToken) {
        d.jqlAcpAsyncUserItems.clear();
        d.jqlAcpAsyncUserError.clear();
        d.jqlAcpUserSearchFireAt = 0.0;
        d.jqlAcpUserSearchQuery.clear();
        d.jqlAcpUserSearchInFlightId = 0;
        return;
    }
    const std::string& q = meta.UserSearchPrefix;
    if (q.size() < 2) {
        d.jqlAcpAsyncUserItems.clear();
        d.jqlAcpAsyncUserError.clear();
        d.jqlAcpUserSearchFireAt = 0.0;
        d.jqlAcpUserSearchQuery.clear();
        d.jqlAcpUserSearchInFlightId = 0;
        return;
    }
    if (q != d.jqlAcpUserSearchQuery) {
        ++d.jqlAcpUserSearchRequestId;
        d.jqlAcpUserSearchQuery = q;
        d.jqlAcpUserSearchArmedId = d.jqlAcpUserSearchRequestId;
        d.jqlAcpUserSearchFireAt = ImGui::GetTime() + 0.22;
        d.jqlAcpAsyncUserItems.clear();
        d.jqlAcpAsyncUserError.clear();
        return;
    }
    // Poll any pending future first — non-blocking; consume result when ready and matching id.
    if (d.jqlAcpUserSearchFuture.valid() &&
        d.jqlAcpUserSearchFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        UiDrawSession::JqlUserSearchResult result = d.jqlAcpUserSearchFuture.get();
        const uint64_t completedId = d.jqlAcpUserSearchInFlightId;
        d.jqlAcpUserSearchInFlightId = 0;
        // Drop stale results when the user typed past this request (armed id moved on).
        if (completedId == d.jqlAcpUserSearchArmedId) {
            if (!result.Ok) {
                d.jqlAcpAsyncUserError = result.Error;
                d.jqlAcpAsyncUserItems.clear();
            } else {
                d.jqlAcpAsyncUserError.clear();
                d.jqlAcpAsyncUserItems.clear();
                std::unordered_set<std::string> seen;
                for (const auto& u : result.Users) {
                    if (u.AccountId.empty()) {
                        continue;
                    }
                    const std::string ins = InsertTokenForUserAccountId(u.AccountId);
                    if (!seen.insert(ins).second) {
                        continue;
                    }
                    std::string label = u.DisplayName.empty() ? u.AccountId : (u.DisplayName + " -> " + ins);
                    d.jqlAcpAsyncUserItems.push_back(QuerySuggestion{std::move(label), ins});
                    if (static_cast<int>(d.jqlAcpAsyncUserItems.size()) >= 40) {
                        break;
                    }
                }
            }
        }
    }
    if (d.jqlAcpUserSearchFireAt <= 0.0) {
        return;
    }
    const double now = ImGui::GetTime();
    if (now < d.jqlAcpUserSearchFireAt) {
        return;
    }
    if (d.jqlAcpUserSearchArmedId != d.jqlAcpUserSearchRequestId) {
        d.jqlAcpUserSearchFireAt = 0.0;
        return;
    }
    if (d.jqlAcpUserSearchInFlightId != 0) {
        // Already issued for this request id; let it complete (poll above next frame).
        return;
    }

    // Dispatch to worker thread via std::async — Jira/Plane user search HTTP must not block
    // the UI thread (Pillar 2 — finding #3). `AppController&` outlives any UI frame so the
    // ref capture is safe; the future is consumed on the UI thread above.
    const std::string capturedQ = q;
    const uint64_t inFlightId = d.jqlAcpUserSearchArmedId;
    d.jqlAcpUserSearchInFlightId = inFlightId;
    d.jqlAcpUserSearchFireAt = 0.0;
    d.jqlAcpUserSearchFuture = std::async(std::launch::async, [&app, capturedQ]() {
        UiDrawSession::JqlUserSearchResult r;
        r.Ok = app.SearchUsersByQuery(capturedQ, r.Users, r.Error);
        return r;
    });
}
