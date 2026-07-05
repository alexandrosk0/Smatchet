#include "SmatchetGridUiSupport.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"

#include "AppController.h"
#include "AnnotateAnalysisUi.h"
#include "ConfigManager.h"
#include "SmatchetInputModifierBridge.h"
#include "StringUtil.h"
#include "Ui/SmatchetUserInfoUi.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string JoinCsvLocal(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += values[i];
    }
    return out;
}

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
static void DrawLuaTicketActionMenuItems(AppController* app, const UiDrawSession* ui, const std::string& issueKey) {
    if (!app || !ui || issueKey.empty()) {
        return;
    }
    const auto luaActions = app->GetLuaTicketActionNames();
    if (luaActions.empty()) {
        return;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Lua Actions");
    for (const auto& name : luaActions) {
        if (ImGui::MenuItem(name.c_str())) {
            app->ExecuteLuaTicketAction(name, issueKey);
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.lua_action", "Lua Action"),
                SmatchetLocalization::Format("toast.lua_queued", "Queued: %s", name.c_str()), ToastType::Success);
        }
    }
}
#endif

static std::string ReplaceStringPlaceholder(std::string str, const std::string& placeholder,
                                            const std::string& replacement) {
    size_t pos = 0;
    while ((pos = str.find(placeholder, pos)) != std::string::npos) {
        str.replace(pos, placeholder.length(), replacement);
        pos += replacement.length();
    }
    return str;
}

static std::string ResolveCommentTemplate(std::string text, const std::string& issueKey) {
    text = ReplaceStringPlaceholder(text, "{key}", issueKey);
    text = ReplaceStringPlaceholder(text, "{issueKey}", issueKey);
    return text;
}

static std::string BuildTemplateCommentBody(const std::string& issueKey, const std::string& templateId,
                                            const std::vector<CommentTemplate>& templates) {
    auto it =
        std::find_if(templates.begin(), templates.end(), [&templateId](const auto& t) { return t.Id == templateId; });
    if (it != templates.end()) {
        return ResolveCommentTemplate(it->Text, issueKey);
    }
    if (templateId == "need_repro") {
        return ResolveCommentTemplate("Need reproduction details for {key}:\n- Repro steps\n- Expected vs actual "
                                      "result\n- Branch / CL / build\n- Environment details",
                                      issueKey);
    }
    if (templateId == "need_logs") {
        return ResolveCommentTemplate("Please attach diagnostic data for {key}:\n- Relevant logs\n- Callstack / crash "
                                      "context\n- Local repro notes",
                                      issueKey);
    }
    return ResolveCommentTemplate("Triage handoff for {key}:\n- Current owner: \n- Next action: \n- ETA: \n- Blockers:",
                                  issueKey);
}

static void DrawAnnotateFromCallstackMenuIfAny(AppController* app, UiDrawSession* ui, const CachedTicket* row,
                                               const std::string& issueKey) {
    if (!app || !ui || !row || issueKey.empty()) {
        return;
    }
    if (!AnnotateRowHasNonEmptyCallstackField(*app, *row)) {
        return;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Annotate...")) {
        // pane(): the pane whose window hosts this right-click popup (Slice 2).
        OpenAnnotateAnalysisForGridIssue(*app, ui->showAnnotateAnalysis, ui->pane().gridState, issueKey);
    }
}

const TrackerUser* FindUserMatchingValue(AppController& app, const std::string& value) {
    for (const auto& u : app.GetAvailableUsers()) {
        if (u.DisplayName == value || u.AccountId == value || u.EmailAddress == value) {
            return &u;
        }
    }
    return nullptr;
}

// "User Info..." on user-type cells (assignee/reporter/...): opens the dockable
// User Info window targeted at the cell's user (user-info-window.md, Slice 5).
static void DrawUserInfoMenuItemIfUserField(AppController* app, UiDrawSession* ui, const std::string& fieldId,
                                            const std::string& rawValue) {
    if (!app || !ui || fieldId.empty() || rawValue.empty()) {
        return;
    }
    const TrackerField* field = app->FindFieldById(fieldId);
    if (!field || !field->IsUserType) {
        return;
    }
    ImGui::Separator();
    if (!ImGui::MenuItem("User Info...")) {
        return;
    }
    std::string value = TrimCopy(rawValue);
    const TrackerUser* match = FindUserMatchingValue(*app, value);
    if (!match) {
        // Multi-user cells render comma-joined — retry with the first entry.
        const size_t comma = value.find(',');
        if (comma != std::string::npos) {
            value = TrimCopy(value.substr(0, comma));
            match = FindUserMatchingValue(*app, value);
        }
    }
    // No catalog match (offline / stale catalog): open with the raw cell text as
    // best-effort identity — VCS feeds still work off the email/name strings.
    const std::string displayName = match ? match->DisplayName : value;
    const std::string email = match ? match->EmailAddress : std::string();
    const std::string accountId = match ? match->AccountId : value;
    SmatchetUserInfoUi::Open(*ui, ui->pane().id, displayName, email, accountId);
}

} // namespace

/**
 * Right-click on the cell group: Copy (plain RMB). Shift+RMB: full raw cached value panel.
 * Uses OpenPopup — not BeginPopupContextItem — so Shift+RMB is not swallowed by the Copy menu.
 */
void DrawGridCellRightClickPopups(const std::string& imguiStackId, const std::string& issueKey,
                                  const std::string& fieldId, const std::string& fieldLabel,
                                  const std::string& rawValue, const std::string& richValue, AppController* app,
                                  UiDrawSession* ui, bool readOnlyMode, const CachedTicket* rowForAnnotateMenu) {
    ImGui::PushID(imguiStackId.c_str());
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
        if (ImGui::GetIO().KeyShift) {
            ImGui::SetNextWindowSize(ImVec2(520.0f, 300.0f), ImGuiCond_FirstUseEver);
            ImGui::OpenPopup("cell_raw_cached");
        } else {
            ImGui::OpenPopup("cell_copy_quick");
        }
    }
    if (ImGui::BeginPopup("cell_raw_cached")) {
        ImGui::TextUnformatted("Raw cached value");
        ImGui::Separator();
        ImGui::Text("Issue: %s", issueKey.c_str());
        if (!fieldId.empty()) {
            ImGui::Text("Field: %s (%s)", fieldLabel.c_str(), fieldId.c_str());
        } else {
            ImGui::Text("Field: %s", fieldLabel.c_str());
        }
        ImGui::TextUnformatted("Value:");
        if (rawValue.empty()) {
            ImGui::TextDisabled("-");
        } else {
            ImGui::BeginChild("cell_raw_cached_body", ImVec2(0, 140.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
            ImGui::TextUnformatted(rawValue.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        if (ImGui::MenuItem("Copy value")) {
            ImGui::SetClipboardText(rawValue.c_str());
        }
        DrawAnnotateFromCallstackMenuIfAny(app, ui, rowForAnnotateMenu, issueKey);
        DrawUserInfoMenuItemIfUserField(app, ui, fieldId, rawValue);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        DrawLuaTicketActionMenuItems(app, ui, issueKey);
#endif
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("cell_copy_quick")) {
        if (ImGui::MenuItem("Copy")) {
            ImGui::SetClipboardText(rawValue.c_str());
        }
        // "Copy raw" — copies the unprocessed backend value. For ADF (Jira description /
        // environment / custom doc fields) and HTML (Plane description) the rich payload is
        // captured separately in CachedTicket.fieldRichValues and surfaced here. For all other
        // fields the cached display value already IS the raw backend value, so we fall back to
        // rawValue and the menu item still works as expected.
        const std::string& rawForCopy = !richValue.empty() ? richValue : rawValue;
        if (ImGui::MenuItem("Copy raw")) {
            ImGui::SetClipboardText(rawForCopy.c_str());
        }
        DrawAnnotateFromCallstackMenuIfAny(app, ui, rowForAnnotateMenu, issueKey);
        DrawUserInfoMenuItemIfUserField(app, ui, fieldId, rawValue);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        DrawLuaTicketActionMenuItems(app, ui, issueKey);
#endif
        if (app && ui && fieldId.empty() && !issueKey.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Quick comment templates");
            if (readOnlyMode) {
                ImGui::TextDisabled("(disabled while offline/read-only)");
            } else {
                for (const auto& t : ui->cfg.QuickCommentTemplates) {
                    if (ImGui::MenuItem(t.Title.c_str())) {
                        // Dispatch the Jira comment POST to a worker thread so the right-click
                        // menu doesn't block the UI for the POST latency (Pillar 2 — finding #4).
                        // Capture the issueKey + body by value; AppController& lives for the
                        // lifetime of the app. The acknowledgement + completion toasts are
                        // posted back to the UI thread via MainThreadDispatcher.
                        const std::string capturedIssueKey = issueKey;
                        const std::string commentBody =
                            BuildTemplateCommentBody(issueKey, t.Id, ui->cfg.QuickCommentTemplates);
                        AppController* appPtr = app;
                        SmatchetToastManager::Instance().Push(
                            SmatchetLocalization::T("comments.queued_title", "Comment Queued"),
                            SmatchetLocalization::Format("comments.posting_to", "Posting to %s",
                                                         capturedIssueKey.c_str()),
                            ToastType::Info);
                        app->LaunchBackgroundTask([appPtr, capturedIssueKey, commentBody]() {
                            std::string err;
                            const bool ok = appPtr->AddIssueCommentPlain(capturedIssueKey, commentBody, err);
                            appPtr->mainThreadDispatcher.PostToMainThread([ok, err, capturedIssueKey]() {
                                if (ok) {
                                    SmatchetToastManager::Instance().Push(
                                        SmatchetLocalization::T("toast.comment_posted", "Comment Posted"),
                                        SmatchetLocalization::Format("comments.added_to", "Added to %s",
                                                                     capturedIssueKey.c_str()),
                                        ToastType::Success);
                                } else {
                                    SmatchetToastManager::Instance().Push(
                                        SmatchetLocalization::T("toast.comment_failed", "Comment Failed"),
                                        err.empty() ? SmatchetLocalization::T("toast.failed_jira_comment",
                                                                              "Failed to post Jira comment.")
                                                    : err.c_str(),
                                        ToastType::Error);
                                }
                            });
                        });
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void DrawTicketGridHeaderContextMenu(const TicketGridColumn& col, const TrackerField* meta) {
    // Power-user only: Shift + right-click on header (plain RMB keeps default table header behavior).
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetIO().KeyShift) {
        ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(520.0f, 420.0f), ImGuiCond_FirstUseEver);
        ImGui::OpenPopup("grid_hdr_meta");
    }
    if (!ImGui::BeginPopup("grid_hdr_meta")) {
        return;
    }

    if (col.ColumnKind == TicketGridColumn::Kind::Id) {
        ImGui::TextUnformatted("Issue key column (not a Jira field)");
        ImGui::Separator();
        ImGui::Text("Key: %s", col.Key.c_str());
        ImGui::Text("Label: %s", col.Label.c_str());
        if (ImGui::MenuItem("Copy column key")) {
            ImGui::SetClipboardText(col.Key.c_str());
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted("Jira field (catalog)");
    ImGui::Separator();

    const std::string& fieldIdForCopy = meta ? meta->Id : col.FieldId;
    if (ImGui::MenuItem("Copy field id")) {
        ImGui::SetClipboardText(fieldIdForCopy.c_str());
    }

    if (meta) {
        ImGui::Text("Id: %s", meta->Id.c_str());
        ImGui::Text("Name: %s", meta->Name.c_str());
        ImGui::Text("Type: %s", meta->Type.c_str());
        ImGui::Text("ReadOnly: %s", meta->ReadOnly ? "true" : "false");
        ImGui::Text("IsCustom: %s", meta->IsCustom ? "true" : "false");
        ImGui::Text("IsArray: %s", meta->IsArray ? "true" : "false");
        ImGui::Text("ItemsType: %s", meta->ItemsType.empty() ? "(none)" : meta->ItemsType.c_str());
        ImGui::Text("IsUserType: %s", meta->IsUserType ? "true" : "false");
        ImGui::Text("AllowedValues count: %d", static_cast<int>(meta->AllowedValues.size()));
        ImGui::Text("AllowedValueOptions count: %d", static_cast<int>(meta->AllowedValueOptions.size()));

        const int optCount = static_cast<int>(meta->AllowedValueOptions.size());
        if (optCount > 0) {
            ImGui::Separator();
            ImGui::TextUnformatted("AllowedValueOptions (scroll)");
            ImGui::BeginChild("hdr_allowed_opts", ImVec2(0, 100.0f), true);
            const int show = (std::min)(optCount, 200);
            for (int i = 0; i < show; ++i) {
                const TrackerFieldOption& o = meta->AllowedValueOptions[static_cast<size_t>(i)];
                ImGui::BulletText("id=%s  value=%s", o.Id.c_str(), o.Value.c_str());
            }
            if (optCount > 200) {
                ImGui::TextDisabled("... %d more", optCount - 200);
            }
            ImGui::EndChild();
        }

        const int valCount = static_cast<int>(meta->AllowedValues.size());
        if (valCount > 0 && meta->AllowedValueOptions.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("AllowedValues (scroll)");
            ImGui::BeginChild("hdr_allowed_vals", ImVec2(0, 80.0f), true);
            const int show = (std::min)(valCount, 200);
            for (int i = 0; i < show; ++i) {
                ImGui::BulletText("%s", meta->AllowedValues[static_cast<size_t>(i)].c_str());
            }
            if (valCount > 200) {
                ImGui::TextDisabled("... %d more", valCount - 200);
            }
            ImGui::EndChild();
        }
    } else {
        ImGui::TextDisabled("No catalog entry for field id: %s", col.FieldId.c_str());
        ImGui::Text("Column key: %s", col.Key.c_str());
        ImGui::Text("Header label: %s", col.Label.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("GET /rest/api/3/field (raw object)");
    if (meta && !meta->RawFieldDefinitionJson.empty()) {
        if (ImGui::MenuItem("Copy raw JSON")) {
            ImGui::SetClipboardText(meta->RawFieldDefinitionJson.c_str());
        }
        ImGui::BeginChild("hdr_raw_json", ImVec2(0, 140.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(meta->RawFieldDefinitionJson.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("(not available — refresh field catalog or synthetic field)");
    }

    ImGui::EndPopup();
}

std::string GetCellRawForCopy(const CachedTicket& ticket, const TicketGridColumn& column,
                              const TrackerField* fieldMeta) {
    if (column.ColumnKind == TicketGridColumn::Kind::Id) {
        return ticket.id;
    }
    const std::string raw = ticket.GetFieldValue(column.FieldId);
    if (IsTrackerDateOrDateTimeField(column.FieldId, fieldMeta)) {
        return DisplayValueForTrackerDateField(column.FieldId, fieldMeta, raw);
    }
    return raw;
}

void CopyGridRectAsTsv(const std::vector<CachedTicket>& tickets, const std::vector<size_t>& sortedIdx,
                       const std::vector<TicketGridColumn>& columns, const TrackerFieldCatalogIndex& catalog,
                       const GridRectSelection& sel) {
    if (!sel.HasAnySelection() || columns.empty()) {
        return;
    }
    const int lastColIdx = static_cast<int>(columns.size()) - 1;
    const bool useSorted = !sortedIdx.empty();

    // Union of rows touched by either the rectangle or the whole-row set.
    // Using std::set keeps the output in ascending sort-order, matching the
    // visual order in the grid.
    std::set<int> allRows;
    if (sel.Active) {
        for (int r = sel.MinRow(); r <= sel.MaxRow(); ++r) {
            if (r >= 0)
                allRows.insert(r);
        }
    }
    for (int r : sel.Rows) {
        if (r >= 0)
            allRows.insert(r);
    }

    std::string out;
    for (int r : allRows) {
        const size_t logicalRow = static_cast<size_t>(r);
        size_t ticketIdx = logicalRow;
        if (useSorted) {
            if (logicalRow >= sortedIdx.size())
                continue;
            ticketIdx = sortedIdx[logicalRow];
        }
        if (ticketIdx >= tickets.size())
            continue;
        const CachedTicket& t = tickets[ticketIdx];

        int c0 = -1;
        int c1 = -1;
        if (sel.RowSelected(r)) {
            c0 = 0;
            c1 = lastColIdx;
        } else if (sel.RectContains(r, sel.MinCol())) {
            c0 = sel.MinCol();
            c1 = sel.MaxCol();
        }
        if (c0 < 0 || c1 < c0)
            continue;

        for (int c = c0; c <= c1; ++c) {
            if (c < 0 || c >= static_cast<int>(columns.size()))
                continue;
            const TicketGridColumn& col = columns[static_cast<size_t>(c)];
            const TrackerField* meta =
                (col.ColumnKind == TicketGridColumn::Kind::Id) ? nullptr : catalog.Find(col.FieldId);
            out += SanitizeForSpreadsheet(GetCellRawForCopy(t, col, meta));
            if (c < c1)
                out.push_back('\t');
        }
        out.push_back('\n');
    }
    ImGui::SetClipboardText(out.c_str());
}

// Derives a session-local identifier for the current sort/tickets state; used
// to invalidate the rectangular selection when the row order or contents
// change (sort re-applied, tickets reloaded, etc.).
std::uint64_t ComputeGridSortSignature(const std::string& sortFingerprint, std::uint64_t ticketsRevision,
                                       std::size_t ticketCount) {
    auto mix = [](std::uint64_t h, std::uint64_t v) {
        v += 0x9e3779b97f4a7c15ULL;
        v ^= h;
        v *= 0xff51afd7ed558ccdULL;
        v ^= v >> 33;
        return v;
    };
    std::uint64_t h = std::hash<std::string>{}(sortFingerprint);
    h = mix(h, static_cast<std::uint64_t>(ticketsRevision));
    h = mix(h, static_cast<std::uint64_t>(ticketCount));
    if (h == 0)
        h = 1; // reserve 0 for "cleared"
    return h;
}

void GridSelectAllRows(GridPane& pane, const std::vector<CachedTicket>& tickets) {
    GridRectSelection& sel = pane.gridState.RectSel;
    sel.ClearAll();
    const size_t rowCount = !pane.filteredIndices.empty() ? pane.filteredIndices.size() : tickets.size();
    for (size_t row = 0; row < rowCount; ++row) {
        sel.Rows.insert(static_cast<int>(row));
    }
    if (rowCount > 0) {
        sel.PrimaryRow = 0;
        sel.SortSignature =
            ComputeGridSortSignature(pane.cachedSortFingerprint, pane.cachedSortTicketsRevision, tickets.size());
        const size_t firstTicketIndex = !pane.filteredIndices.empty() ? pane.filteredIndices.front() : 0;
        if (firstTicketIndex < tickets.size()) {
            pane.gridState.ActiveIssueId = tickets[firstTicketIndex].id;
        }
    }
}

std::string BuildGridContextSignature(const ViewDefinition* view, const std::string& jqlQuery) {
    std::string s;
    if (!view) {
        s = "@\x1e";
    } else {
        s = view->Id;
        s.push_back('\x1e');
        s += JoinCsvLocal(view->Fields);
        s.push_back('\x1e');
        s += JoinCsvLocal(view->ColumnOrder);
        s.push_back('\x1e');
    }
    s += jqlQuery;
    return s;
}

void CancelUnfinishedNewIssueForGridChange(UiDrawSession& d) {
    if (!d.newIssueDraftActive && !d.newIssueCreateInFlight) {
        return;
    }
    d.newIssueDraftActive = false;
    d.newIssueScrollDraftRowIntoViewPending = false;
    d.newIssueFocusSummaryPending = false;
    d.newIssueDraft = IssueDraft{};
    d.newIssueDraftEditBufs.clear();
    d.newIssueMissingFieldIds.clear();
    d.newIssueQueueFallbackVisible = false;
    d.newIssueQueueFallbackError.clear();
    if (d.newIssueCreateInFlight) {
        d.newIssueDiscardAsyncCreateResult = true;
    }
}

#if defined(_WIN32)
bool ImGuiEffectiveKeyCtrl() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool pushed = SmatchetInput_GetPluginModifiersPushedCtrl();
    const bool async =
        ((::GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
    return io.KeyCtrl || pushed || async;
}
bool ImGuiEffectiveKeyShift() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool pushed = SmatchetInput_GetPluginModifiersPushedShift();
    const bool async =
        ((::GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) || ((::GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
    return io.KeyShift || pushed || async;
}
#else
bool ImGuiEffectiveKeyCtrl() { return ImGui::GetIO().KeyCtrl; }
bool ImGuiEffectiveKeyShift() { return ImGui::GetIO().KeyShift; }
#endif

std::string BuildCellKey(const std::string& issueId, const std::string& fieldId) { return issueId + "|" + fieldId; }

std::string SanitizeClipboardCell(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

void SyncWithCurrentView(AppController& app, UiDrawSession& d, const ViewsStore& store, bool pushHistory) {
    ConfigManager::Save(d.cfg);
    if (pushHistory)
        d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
    app.SyncWithBackend(&d.cfg, &store);
}
