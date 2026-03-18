#include "SmatchetUI.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "SpreadsheetState.h"
#include "AiController.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring> // for strncpy
#include <algorithm>
#include <deque>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdlib>
#include <cstdio>

namespace {
std::string FormatNetworkBytes(std::uint64_t n) {
    if (n < 1024u) {
        return std::to_string(n) + " B";
    }
    const double kb = static_cast<double>(n) / 1024.0;
    if (kb < 1024.0) {
        char b[48];
        std::snprintf(b, sizeof(b), "%.2f KB", kb);
        return std::string(b);
    }
    char b[48];
    std::snprintf(b, sizeof(b), "%.2f MB", kb / 1024.0);
    return std::string(b);
}

std::string JoinCsv(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += values[i];
    }
    return out;
}

std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            size_t start = 0;
            size_t end = current.size();
            while (start < end && (current[start] == ' ' || current[start] == '\t')) ++start;
            while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t')) --end;
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
    while (start < end && (current[start] == ' ' || current[start] == '\t')) ++start;
    while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t')) --end;
    if (end > start) {
        result.push_back(current.substr(start, end - start));
    }
    return result;
}

bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }

    auto toLower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string loweredText(text.size(), '\0');
    std::string loweredNeedle(needle.size(), '\0');
    std::transform(text.begin(), text.end(), loweredText.begin(), toLower);
    std::transform(needle.begin(), needle.end(), loweredNeedle.begin(), toLower);
    return loweredText.find(loweredNeedle) != std::string::npos;
}

std::vector<std::string> ToSortedVector(const std::unordered_set<std::string>& values) {
    std::vector<std::string> result(values.begin(), values.end());
    std::sort(result.begin(), result.end());
    return result;
}

// Natural issue key comparison: BLOOP-2 < BLOOP-12 (project lexicographic, then number).
bool CompareIssueKeyNatural(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) -> std::pair<std::string, long long> {
        std::string project;
        long long num = 0;
        const size_t dash = s.rfind('-');
        if (dash != std::string::npos && dash + 1 < s.size()) {
            project = s.substr(0, dash);
            const std::string tail = s.substr(dash + 1);
            if (!tail.empty()) {
                char* end = nullptr;
                const long long v = std::strtoll(tail.c_str(), &end, 10);
                if (end == tail.c_str() + tail.size()) {
                    num = v;
                    return {project, num};
                }
            }
        }
        return {s, 0};
    };
    const auto pa = split(a);
    const auto pb = split(b);
    if (pa.first != pb.first) {
        return pa.first < pb.first;
    }
    return pa.second < pb.second;
}

// Parse "10h", "2d 4h", "36000" to seconds for sort. Same work units as display (8h day, 5d week).
long long ParseDurationToSecondsForSort(const std::string& input) {
    const long long kSecondsPerHour = 3600LL;
    const long long kSecondsPerDay = 8LL * kSecondsPerHour;
    const long long kSecondsPerWeek = 5LL * kSecondsPerDay;
    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };
    std::string s = trim(input);
    if (s.empty()) return 0;
    size_t pos = 0;
    try {
        long long v = std::stoll(s, &pos, 10);
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
        if (pos >= s.size()) return v;
    } catch (...) {}
    pos = 0;
    long long total = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
        if (pos >= s.size()) break;
        long long num = 1;
        if (std::isdigit(static_cast<unsigned char>(s[pos]))) {
            size_t next = 0;
            try {
                num = std::stoll(s.substr(pos), &next, 10);
                pos += next;
            } catch (...) { break; }
        }
        if (pos >= s.size()) { total += num; break; }
        const char u = s[pos];
        if (u == 'w' || u == 'W') { total += num * kSecondsPerWeek; ++pos; }
        else if (u == 'd' || u == 'D') { total += num * kSecondsPerDay; ++pos; }
        else if (u == 'h' || u == 'H') { total += num * kSecondsPerHour; ++pos; }
        else if (u == 'm' || u == 'M') { total += num * 60LL; ++pos; }
        else { total += num; }
    }
    return total;
}

static const std::unordered_set<std::string> kTimeTrackingFieldIds = {
    "timeoriginalestimate", "timeestimate", "timespent",
    "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent"
};

static const std::unordered_set<std::string> kDateFieldIds = {
    "created", "updated", "duedate"
};

// Returns -1 if a < b, 0 if equal, 1 if a > b. Empty values sort last when ascending.
int CompareFieldValuesForSort(const std::string& fieldId,
                              const JiraField* fieldMeta,
                              const std::string& aVal,
                              const std::string& bVal,
                              int sortDirection) {
    const bool aEmpty = aVal.empty();
    const bool bEmpty = bVal.empty();
    if (aEmpty && bEmpty) return 0;
    if (aEmpty) return (sortDirection == 1) ? 1 : -1;  // ascending: empty last
    if (bEmpty) return (sortDirection == 1) ? -1 : 1;

    if (kTimeTrackingFieldIds.count(fieldId)) {
        const long long sa = ParseDurationToSecondsForSort(aVal);
        const long long sb = ParseDurationToSecondsForSort(bVal);
        if (sa != sb) return (sa < sb) ? -1 : 1;
        return 0;
    }
    if (kDateFieldIds.count(fieldId) || (fieldMeta && (fieldMeta->Type == "date" || fieldMeta->Type == "datetime"))) {
        const int cmp = aVal.compare(bVal);
        if (cmp != 0) return (cmp < 0) ? -1 : 1;
        return 0;
    }
    if (fieldMeta && fieldMeta->Type == "number") {
        bool aNum = false, bNum = false;
        double da = 0, db = 0;
        try { da = std::stod(aVal); aNum = true; } catch (...) {}
        try { db = std::stod(bVal); bNum = true; } catch (...) {}
        if (aNum && bNum) {
            if (da != db) return (da < db) ? -1 : 1;
            return 0;
        }
    } else {
        try {
            size_t posA = 0, posB = 0;
            const long long na = std::stoll(aVal, &posA, 10);
            const long long nb = std::stoll(bVal, &posB, 10);
            if (posA == aVal.size() && posB == bVal.size()) {
                if (na != nb) return (na < nb) ? -1 : 1;
                return 0;
            }
        } catch (...) {}
    }
    auto ciCompare = [](const std::string& x, const std::string& y) {
        const size_t n = std::min(x.size(), y.size());
        for (size_t i = 0; i < n; ++i) {
            const int cxa = std::tolower(static_cast<unsigned char>(x[i]));
            const int cxb = std::tolower(static_cast<unsigned char>(y[i]));
            if (cxa != cxb) return (cxa < cxb) ? -1 : 1;
        }
        if (x.size() != y.size()) return (x.size() < y.size()) ? -1 : 1;
        return 0;
    };
    return ciCompare(aVal, bVal);
}

struct JiraFieldCatalogIndex {
    explicit JiraFieldCatalogIndex(const std::vector<JiraField>& fields) {
        for (const auto& field : fields) {
            FieldById[field.Id] = &field;
        }
    }

    const JiraField* Find(const std::string& fieldId) const {
        const auto it = FieldById.find(fieldId);
        return it == FieldById.end() ? nullptr : it->second;
    }

    std::string DisplayName(const std::string& fieldId) const {
        if (fieldId == "history") {
            return "History";
        }
        const JiraField* field = Find(fieldId);
        return field ? field->Name : fieldId;
    }

private:
    std::unordered_map<std::string, const JiraField*> FieldById;
};

struct TicketGridColumn {
    enum class Kind {
        Id,
        JiraFieldValue
    };

    Kind ColumnKind = Kind::Id;
    std::string Key;
    std::string Label;
    std::string FieldId;
};

class TicketGridColumnsBuilder {
public:
    static std::vector<TicketGridColumn> Build(const ViewDefinition& view, const JiraFieldCatalogIndex& catalog) {
        std::vector<TicketGridColumn> columns;
        std::vector<TicketGridColumn> allColumns;
        allColumns.push_back({TicketGridColumn::Kind::Id, "id", "ID", std::string()});

        std::unordered_set<std::string> seenFieldIds;
        for (const auto& rawFieldId : view.Fields) {
            const std::string fieldId = TrimFieldId(rawFieldId);
            if (fieldId.empty() || !seenFieldIds.insert(fieldId).second) {
                continue;
            }

            TicketGridColumn column;
            column.ColumnKind = TicketGridColumn::Kind::JiraFieldValue;
            column.Key = "field:" + fieldId;
            column.FieldId = fieldId;
            column.Label = catalog.DisplayName(fieldId);
            allColumns.push_back(column);
        }

        std::unordered_map<std::string, TicketGridColumn> byKey;
        for (const auto& col : allColumns) {
            byKey[col.Key] = col;
        }

        std::unordered_set<std::string> usedKeys;
        for (const auto& key : view.ColumnOrder) {
            const auto it = byKey.find(key);
            if (it == byKey.end()) {
                continue;
            }
            columns.push_back(it->second);
            usedKeys.insert(key);
        }
        for (const auto& col : allColumns) {
            if (usedKeys.find(col.Key) == usedKeys.end()) {
                columns.push_back(col);
            }
        }

        return columns;
    }

private:
    static std::string TrimFieldId(const std::string& value) {
        size_t start = 0;
        size_t end = value.size();
        while (start < end && (value[start] == ' ' || value[start] == '\t' || value[start] == '\n' || value[start] == '\r')) {
            ++start;
        }
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\n' || value[end - 1] == '\r')) {
            --end;
        }
        return value.substr(start, end - start);
    }
};

struct PendingFieldEdit {
    std::string IssueId;
    JiraField Field;
    std::vector<std::string> Values;
};

struct FieldCatalogFetchResult {
    bool Ok = false;
    std::vector<JiraField> Fields;
    std::vector<JiraComponent> Components;
    std::vector<JiraUser> Users;
    std::string Error;
    std::string Warning;
};

enum class CellWriteState {
    Saving,
    Success,
    Error
};

struct CellWriteFeedback {
    CellWriteState State = CellWriteState::Saving;
    std::string Message;
    int FramesRemaining = 0;
};

struct UiDrawSession {
    bool cfgInitialized = false;
    JiraConfig cfg;

    bool showJiraSettings = false;
    bool showViewsDashboard = true;
    bool showNetworkStats = false;

    bool fieldCatalogFetchStarted = false;
    bool fieldCatalogLoading = false;
    std::future<FieldCatalogFetchResult> fieldCatalogFuture;
    bool triggerCatalogRefetch = false;
    std::string fieldCatalogWarning;

    bool appliedInitialView = false;

    char domainBuf[128]{};
    char emailBuf[128]{};
    char tokenBuf[512]{};
    char projectKeyBuf[64]{};
    bool tooltipOverflowEnabled = false;
    bool jiraBuffersInitialized = false;

    char viewNameBuf[128]{};
    char viewJqlBuf[512]{};
    char selectedFieldsBuf[1024]{};
    char fieldSearchBuf[128]{};
    std::vector<std::string> editingColumnOrder;
    int selectedColumnOrderIndex = -1;
    std::string editingViewId;
    std::vector<std::string> lastSyncedColumnOrder;

    SpreadsheetState gridState;
    std::string gridEditError;
    std::string gridEditSuccess;
    std::deque<PendingFieldEdit> queuedFieldEdits;
    bool hasInFlightEdit = false;
    PendingFieldEdit inFlightEdit;
    int inFlightDelayFrames = 0;
    std::unordered_map<std::string, CellWriteFeedback> cellFeedbackByKey;

    std::string lastGridActiveViewId;

    std::string aiResponse;
    bool aiIsThinking = false;

    std::vector<char> logBuffer;
};

static UiDrawSession g_ui;

std::string BuildCellKey(const std::string& issueId, const std::string& fieldId) {
    return issueId + "|" + fieldId;
}

static std::string BuildJiraBrowseUrl(const JiraConfig& cfg, const std::string& issueKey) {
    if (cfg.Domain.empty() || issueKey.empty()) {
        return std::string();
    }
    std::string base = cfg.Domain;
    if (base.find("http://") != 0 && base.find("https://") != 0) {
        base = "https://" + base;
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/browse/" + issueKey;
}

static void OpenUrlInDefaultBrowser(const std::string& url) {
    if (url.empty()) {
        return;
    }
#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + url + "\"";
    std::system(cmd.c_str());
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\"";
    std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + url + "\"";
    std::system(cmd.c_str());
#endif
}

void RenderClippedFieldText(const std::string& rawValue,
                            float availWidth,
                            bool tooltipsEnabled,
                            bool disabled) {
    const std::string& displayValue = rawValue;

    bool hasNewline = false;
    std::string singleLine = displayValue;
    for (size_t i = 0; i < singleLine.size(); ++i) {
        if (singleLine[i] == '\n' || singleLine[i] == '\r') {
            singleLine.erase(i);
            hasNewline = true;
            break;
        }
    }

    const ImVec2 textSize = ImGui::CalcTextSize(singleLine.c_str());
    const bool horizontallyClipped = (availWidth > 0.0f && textSize.x > availWidth + 1.0f);

    if (disabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    ImGui::TextUnformatted(singleLine.c_str());
    if (disabled) {
        ImGui::PopStyleColor();
    }

    if (tooltipsEnabled && (hasNewline || horizontallyClipped) && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        ImGui::TextUnformatted(displayValue.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

class TicketEditService {
public:
    explicit TicketEditService(AppController& appController) : App(appController) {}

    bool Commit(const PendingFieldEdit& edit, std::string& outError) {
        return App.SubmitJiraFieldEdit(edit.IssueId, edit.Field, edit.Values, outError);
    }

private:
    AppController& App;
};

class TicketFieldEditor {
public:
    static void RenderFieldCell(const CachedTicket& ticket,
                                const JiraField* field,
                                const std::string& currentValue,
                                float availWidth,
                                bool tooltipsEnabled,
                                SpreadsheetState& state,
                                std::vector<PendingFieldEdit>& pendingEdits) {
        if (!field) {
            RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, false);
            return;
        }

        if (field->ReadOnly) {
            RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
            return;
        }

        if (field->IsArray && !field->AllowedValueOptions.empty()) {
            RenderMultiSelectEditor(ticket, *field, currentValue, pendingEdits);
            return;
        }

        if (!field->AllowedValueOptions.empty()) {
            RenderSingleSelectEditor(ticket, *field, currentValue, pendingEdits);
            return;
        }

        RenderTextEditor(ticket, *field, currentValue, state, pendingEdits);
    }

private:
    static void QueueEdit(const std::string& issueId,
                          const JiraField& field,
                          const std::vector<std::string>& values,
                          std::vector<PendingFieldEdit>& pendingEdits) {
        PendingFieldEdit edit;
        edit.IssueId = issueId;
        edit.Field = field;
        edit.Values = values;
        pendingEdits.push_back(std::move(edit));
    }

    static std::string ResolveOptionId(const JiraField& field, const std::string& value) {
        for (const auto& option : field.AllowedValueOptions) {
            if (option.Id == value || option.Value == value) {
                return option.Id;
            }
        }
        return value;
    }

    static std::string ResolveOptionLabel(const JiraField& field, const std::string& value) {
        for (const auto& option : field.AllowedValueOptions) {
            if (option.Id == value || option.Value == value) {
                return option.Value;
            }
        }
        return value;
    }

    static std::vector<std::string> ResolveCurrentSelectionIds(const JiraField& field, const std::string& currentValue) {
        std::vector<std::string> ids;
        for (const auto& part : ParseCsv(currentValue)) {
            const std::string id = ResolveOptionId(field, part);
            if (!id.empty()) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    static std::string BuildSelectionPreview(const JiraField& field, const std::vector<std::string>& selectedIds) {
        if (selectedIds.empty()) {
            return "<none>";
        }
        std::vector<std::string> labels;
        labels.reserve(selectedIds.size());
        for (const auto& id : selectedIds) {
            labels.push_back(ResolveOptionLabel(field, id));
        }
        return JoinCsv(labels);
    }

    static void RenderTextEditor(const CachedTicket& ticket,
                                 const JiraField& field,
                                 const std::string& currentValue,
                                 SpreadsheetState& state,
                                 std::vector<PendingFieldEdit>& pendingEdits) {
        const std::string itemId = "##TextCell_" + ticket.id + "_" + field.Id;
        if (state.IsEditingField(ticket.id, field.Id)) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool submitted = ImGui::InputText(
                itemId.c_str(),
                state.EditBuffer,
                sizeof(state.EditBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.ClearEditing();
            } else if (submitted || ImGui::IsItemDeactivatedAfterEdit()) {
                QueueEdit(ticket.id, field, {state.EditBuffer}, pendingEdits);
                state.ClearEditing();
            }
            return;
        }

        bool hasNewline = false;
        std::string singleLine = currentValue;
        for (size_t i = 0; i < singleLine.size(); ++i) {
            if (singleLine[i] == '\n' || singleLine[i] == '\r') {
                singleLine.erase(i);
                hasNewline = true;
                break;
            }
        }
        const std::string& display = singleLine;
        if (ImGui::Selectable((display + itemId).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                state.StartEditingField(ticket.id, field.Id, currentValue);
            }
        }
    }

    static void RenderSingleSelectEditor(const CachedTicket& ticket,
                                         const JiraField& field,
                                         const std::string& currentValue,
                                         std::vector<PendingFieldEdit>& pendingEdits) {
        const std::string currentId = ResolveOptionId(field, currentValue);
        const std::string preview = ResolveOptionLabel(field, currentId);
        const std::string comboId = "##SingleSelect_" + ticket.id + "_" + field.Id;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(comboId.c_str(),
                              preview.empty() ? "<none>" : preview.c_str(),
                              ImGuiComboFlags_NoArrowButton)) {
            const bool selectedNone = currentId.empty();
            if (ImGui::Selectable("<clear>", selectedNone)) {
                QueueEdit(ticket.id, field, {}, pendingEdits);
            }

            for (const auto& option : field.AllowedValueOptions) {
                const bool isSelected = (option.Id == currentId);
                if (ImGui::Selectable(option.Value.c_str(), isSelected)) {
                    QueueEdit(ticket.id, field, {option.Id}, pendingEdits);
                }
            }
            ImGui::EndCombo();
        }
    }

    static void RenderMultiSelectEditor(const CachedTicket& ticket,
                                        const JiraField& field,
                                        const std::string& currentValue,
                                        std::vector<PendingFieldEdit>& pendingEdits) {
        std::vector<std::string> selectedIds = ResolveCurrentSelectionIds(field, currentValue);
        std::unordered_set<std::string> selectedSet(selectedIds.begin(), selectedIds.end());
        const std::string preview = BuildSelectionPreview(field, selectedIds);
        const std::string comboId = "##MultiSelect_" + ticket.id + "_" + field.Id;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
            if (ImGui::Selectable("<clear all>", selectedSet.empty())) {
                QueueEdit(ticket.id, field, {}, pendingEdits);
            }
            ImGui::Separator();

            for (const auto& option : field.AllowedValueOptions) {
                bool checked = (selectedSet.find(option.Id) != selectedSet.end());
                const std::string optionWidget = option.Value + "##Opt_" + ticket.id + "_" + field.Id + "_" + option.Id;
                if (ImGui::Checkbox(optionWidget.c_str(), &checked)) {
                    if (checked) {
                        selectedSet.insert(option.Id);
                    } else {
                        selectedSet.erase(option.Id);
                    }
                    std::vector<std::string> updated(selectedSet.begin(), selectedSet.end());
                    std::sort(updated.begin(), updated.end());
                    QueueEdit(ticket.id, field, updated, pendingEdits);
                }
            }
            ImGui::EndCombo();
        }
    }
};
}

void SmatchetUI::Draw(AppController& app) {
    if (!g_ui.cfgInitialized) {
        g_ui.cfg = ConfigManager::Load();
        g_ui.cfgInitialized = true;
    }
    ViewState.EnsureLoaded(g_ui.cfg);
    drawEnsureCatalogAndInitialSync(app);
    drawMainMenuBar();
    drawNetworkStatsWindow();
    if (g_ui.showJiraSettings) {
        ImGui::OpenPopup("JiraSetup");
        g_ui.showJiraSettings = false;
    }
    drawJiraCredentialsModal(app);
    drawViewsDashboardWindow(app);
    drawLuaAutomationWindow(app);
    drawActiveProjectWindow(app);
    drawAIAssistantWindow(app);
    drawLogWindow();
}

void SmatchetUI::drawEnsureCatalogAndInitialSync(AppController& app) {
    auto& d = g_ui;
    const auto startCatalogFetch = [&](const JiraConfig& fetchCfg) {
        if (d.fieldCatalogLoading) {
            return;
        }
        d.fieldCatalogLoading = true;
        d.fieldCatalogFetchStarted = true;
        d.fieldCatalogFuture = std::async(std::launch::async, [fetchCfg]() {
            FieldCatalogFetchResult result;
            JiraClient client;
            std::vector<JiraField> fields;
            std::vector<JiraComponent> components;
            std::string error;
            result.Ok = client.FetchFieldCatalog(fetchCfg, fields, components, error);
            if (!result.Ok) {
                result.Error = error;
                return result;
            }

            std::vector<JiraUser> users;
            std::string usersError;
            if (!client.FetchUsers(fetchCfg, users, usersError)) {
                result.Warning = usersError;
            }

            if (!users.empty()) {
                for (auto& field : fields) {
                    if (!field.IsUserType) {
                        continue;
                    }
                    field.AllowedValues.clear();
                    field.AllowedValueOptions.clear();
                    field.AllowedValues.reserve(users.size());
                    field.AllowedValueOptions.reserve(users.size());
                    for (const auto& user : users) {
                        field.AllowedValues.push_back(user.DisplayName);
                        JiraFieldOption option;
                        option.Id = user.AccountId;
                        option.Value = user.DisplayName;
                        field.AllowedValueOptions.push_back(std::move(option));
                    }
                }
            }

            result.Fields = std::move(fields);
            result.Components = std::move(components);
            result.Users = std::move(users);
            return result;
        });
    };

    if ((!d.fieldCatalogFetchStarted || d.triggerCatalogRefetch) && !d.fieldCatalogLoading) {
        d.triggerCatalogRefetch = false;
        startCatalogFetch(d.cfg);
    }

    if (d.fieldCatalogLoading &&
        d.fieldCatalogFuture.valid() &&
        d.fieldCatalogFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        FieldCatalogFetchResult result = d.fieldCatalogFuture.get();
        if (result.Ok) {
            app.SetJiraFieldCatalog(std::move(result.Fields), std::move(result.Components), std::string());
            d.fieldCatalogWarning = result.Warning;
            if (!d.fieldCatalogWarning.empty()) {
                LOG_WARN("SmatchetUI: users fetch warning: %s", d.fieldCatalogWarning.c_str());
            }
        } else {
            app.SetJiraFieldCatalog({}, {}, result.Error.empty() ? std::string("Failed to fetch Jira field catalog.") : result.Error);
            d.fieldCatalogWarning.clear();
        }
        d.fieldCatalogLoading = false;
    }

    if (!d.appliedInitialView) {
        const ViewDefinition* activeView = ViewState.GetActiveView();
        if (activeView) {
            d.cfg.JqlQuery = activeView->Jql;
            d.cfg.SelectedFields = activeView->Fields;
            ConfigManager::Save(d.cfg);
        }
        app.SyncWithBackend();
        d.appliedInitialView = true;
    }
}

void SmatchetUI::drawMainMenuBar() {
    auto& d = g_ui;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Jira Credentials...")) {
                d.showJiraSettings = true;
            }
            if (ImGui::MenuItem("Views Dashboard...")) {
                d.showViewsDashboard = true;
            }
            if (ImGui::MenuItem("Network statistics...")) {
                d.showNetworkStats = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void SmatchetUI::drawNetworkStatsWindow() {
    auto& d = g_ui;
    if (!d.showNetworkStats) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Network statistics", &d.showNetworkStats)) {
        const NetworkUsageSnapshot snap = NetworkUsageTracker::Instance().GetSnapshot();
        ImGui::TextWrapped(
            "HTTP traffic from this session of Smatchet. Downloaded = response bodies; "
            "uploaded = request bodies for POST/PUT, or ~%llu B per Jira GET (estimate).",
            static_cast<unsigned long long>(NetworkUsageTracker::kEstimatedGetUploadBytes));
        ImGui::Separator();
        ImGui::TextUnformatted("Jira API");
        ImGui::BulletText("Requests: %llu", static_cast<unsigned long long>(snap.jiraRequests));
        ImGui::BulletText("Uploaded (approx.): %s", FormatNetworkBytes(snap.jiraUploadBytes).c_str());
        ImGui::BulletText("Downloaded: %s", FormatNetworkBytes(snap.jiraDownloadBytes).c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("OpenAI API");
        ImGui::BulletText("Requests: %llu", static_cast<unsigned long long>(snap.openAiRequests));
        ImGui::BulletText("Uploaded: %s", FormatNetworkBytes(snap.openAiUploadBytes).c_str());
        ImGui::BulletText("Downloaded: %s", FormatNetworkBytes(snap.openAiDownloadBytes).c_str());
        ImGui::Separator();
        if (ImGui::Button("Reset counters")) {
            NetworkUsageTracker::Instance().Reset();
        }
    }
    ImGui::End();
}

void SmatchetUI::drawJiraCredentialsModal(AppController& app) {
    auto& d = g_ui;
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal("JiraSetup", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!d.jiraBuffersInitialized) {
            std::memset(d.domainBuf, 0, sizeof(d.domainBuf));
            std::memset(d.emailBuf, 0, sizeof(d.emailBuf));
            std::memset(d.tokenBuf, 0, sizeof(d.tokenBuf));
            std::memset(d.projectKeyBuf, 0, sizeof(d.projectKeyBuf));
            std::strncpy(d.domainBuf, d.cfg.Domain.c_str(), sizeof(d.domainBuf) - 1);
            std::strncpy(d.emailBuf, d.cfg.Email.c_str(), sizeof(d.emailBuf) - 1);
            std::strncpy(d.tokenBuf, d.cfg.ApiToken.c_str(), sizeof(d.tokenBuf) - 1);
            std::strncpy(d.projectKeyBuf, d.cfg.ProjectKey.c_str(), sizeof(d.projectKeyBuf) - 1);
            d.tooltipOverflowEnabled = d.cfg.EnableFieldOverflowTooltips;
            d.jiraBuffersInitialized = true;
        }

        ImGui::Text("Atlassian Cloud Details");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::InputText("Domain", d.domainBuf, sizeof(d.domainBuf), ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SetItemTooltip("e.g. companyname.atlassian.net");

        ImGui::InputText("Email", d.emailBuf, sizeof(d.emailBuf));
        ImGui::InputText("API Token", d.tokenBuf, sizeof(d.tokenBuf), ImGuiInputTextFlags_Password);
        ImGui::InputText("Project Key", d.projectKeyBuf, sizeof(d.projectKeyBuf), ImGuiInputTextFlags_CharsUppercase);
        ImGui::SetItemTooltip("Used for create meta enrichment, e.g. PROJ");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Checkbox("Show tooltips for clipped fields", &d.tooltipOverflowEnabled);
        ImGui::SetItemTooltip("When enabled, hovering a value that is truncated or multiline will show the full text in a tooltip.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("JQL and field selection moved to the Views dashboard window.");
        if (ImGui::Button("Open Views Dashboard")) {
            d.showViewsDashboard = true;
        }

        if (ImGui::Button("Save & Sync", ImVec2(120, 0))) {
            d.cfg.Domain = d.domainBuf;
            d.cfg.Email = d.emailBuf;
            d.cfg.ApiToken = d.tokenBuf;
            d.cfg.ProjectKey = d.projectKeyBuf;
            d.cfg.EnableFieldOverflowTooltips = d.tooltipOverflowEnabled;

            ConfigManager::Save(d.cfg);
            LOG_INFO("Updated Jira config. Domain='%s', Email='%s'", d.cfg.Domain.c_str(), d.cfg.Email.c_str());
            d.triggerCatalogRefetch = true;
            app.SyncWithBackend();

            d.jiraBuffersInitialized = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            d.jiraBuffersInitialized = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void SmatchetUI::drawViewsDashboardWindow(AppController& app) {
    auto& d = g_ui;
    if (!d.showViewsDashboard) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Views", &d.showViewsDashboard);

    ViewsStore& store = ViewState.GetStoreMutable();
    if (store.Views.empty()) {
        ViewState.EnsureLoaded(d.cfg);
    }

    auto loadBuffersFromView = [&](const ViewDefinition& view) {
        std::memset(d.viewNameBuf, 0, sizeof(d.viewNameBuf));
        std::memset(d.viewJqlBuf, 0, sizeof(d.viewJqlBuf));
        std::memset(d.selectedFieldsBuf, 0, sizeof(d.selectedFieldsBuf));
        std::memset(d.fieldSearchBuf, 0, sizeof(d.fieldSearchBuf));
        std::strncpy(d.viewNameBuf, view.Name.c_str(), sizeof(d.viewNameBuf) - 1);
        std::strncpy(d.viewJqlBuf, view.Jql.c_str(), sizeof(d.viewJqlBuf) - 1);
        const std::string selectedFieldsCsv = JoinCsv(view.Fields);
        std::strncpy(d.selectedFieldsBuf, selectedFieldsCsv.c_str(), sizeof(d.selectedFieldsBuf) - 1);
        d.editingColumnOrder = view.ColumnOrder;
        if (d.editingColumnOrder.empty()) {
            d.editingColumnOrder = {"id"};
            for (const auto& fieldId : view.Fields) {
                d.editingColumnOrder.push_back("field:" + fieldId);
            }
        }
        d.selectedColumnOrderIndex = -1;
        d.editingViewId = view.Id;
        d.lastSyncedColumnOrder = view.ColumnOrder;
    };

    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (activeView) {
        if (d.editingViewId != activeView->Id) {
            loadBuffersFromView(*activeView);
        } else if (activeView->ColumnOrder != d.lastSyncedColumnOrder) {
            loadBuffersFromView(*activeView);
        }
    }

    if (activeView) {
            ImGui::Text("Active View");
            if (ImGui::BeginCombo("##ActiveViewCombo", activeView->Name.c_str())) {
                for (const auto& view : store.Views) {
                    const bool isSelected = (view.Id == activeView->Id);
                    if (ImGui::Selectable(view.Name.c_str(), isSelected)) {
                        if (ViewState.Activate(view.Id)) {
                            const ViewDefinition* nowActive = ViewState.GetActiveView();
                            if (nowActive) {
                                d.cfg.JqlQuery = nowActive->Jql;
                                d.cfg.SelectedFields = nowActive->Fields;
                                ConfigManager::Save(d.cfg);
                                app.SyncWithBackend();
                            }
                        }
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::InputText("View Name", d.viewNameBuf, sizeof(d.viewNameBuf));
            ImGui::InputText("JQL", d.viewJqlBuf, sizeof(d.viewJqlBuf));
            ImGui::SetItemTooltip("Atlassian JQL used when fetching issues.");

            ImGui::Spacing();
            if (ImGui::Button("Apply View & Sync")) {
                ViewDefinition updated = *activeView;
                updated.Name = d.viewNameBuf;
                updated.Jql = d.viewJqlBuf;
                updated.Fields = ParseCsv(d.selectedFieldsBuf);
                updated.ColumnOrder = d.editingColumnOrder;
                if (ViewState.UpdateActive(updated)) {
                    d.cfg.JqlQuery = updated.Jql;
                    d.cfg.SelectedFields = updated.Fields;
                    ConfigManager::Save(d.cfg);
                    app.SyncWithBackend();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Create New View")) {
                ViewDefinition created = *activeView;
                created.Name = std::string(d.viewNameBuf).empty() ? std::string("New View") : std::string(d.viewNameBuf);
                created.Jql = d.viewJqlBuf;
                created.Fields = ParseCsv(d.selectedFieldsBuf);
                created.ColumnOrder = d.editingColumnOrder;
                created.Id.clear();
                ViewState.Create(created);
                const ViewDefinition* nowActive = ViewState.GetActiveView();
                if (nowActive) {
                    loadBuffersFromView(*nowActive);
                }
            }
            ImGui::SameLine();
            if (store.Views.size() <= 1) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Delete View")) {
                if (ViewState.DeleteActive()) {
                    const ViewDefinition* nowActive = ViewState.GetActiveView();
                    if (nowActive) {
                        loadBuffersFromView(*nowActive);
                        d.cfg.JqlQuery = nowActive->Jql;
                        d.cfg.SelectedFields = nowActive->Fields;
                        ConfigManager::Save(d.cfg);
                        app.SyncWithBackend();
                    }
                }
            }
            if (store.Views.size() <= 1) {
                ImGui::EndDisabled();
            }

            const std::string& catalogError = app.GetJiraFieldCatalogError();
            if (!catalogError.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s", catalogError.c_str());
                ImGui::PopStyleColor();
            }
            if (!d.fieldCatalogWarning.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.25f, 1.0f));
                ImGui::TextWrapped("%s", d.fieldCatalogWarning.c_str());
                ImGui::PopStyleColor();
            }
            if (d.fieldCatalogLoading) {
                ImGui::TextDisabled("Loading available fields...");
            }

            ImGui::Separator();
            ImGui::Text("Field Picker");
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint("##ViewFieldSearch", "Search by field id or name", d.fieldSearchBuf, sizeof(d.fieldSearchBuf));

            std::unordered_set<std::string> selectedFieldSet;
            for (const auto& fieldId : ParseCsv(d.selectedFieldsBuf)) {
                selectedFieldSet.insert(fieldId);
            }

            const auto& availableFields = app.GetAvailableJiraFields();
            std::vector<const JiraField*> visibleFields;
            std::vector<const JiraField*> systemFields;
            std::vector<const JiraField*> customFields;
            std::vector<const JiraField*> basicFields;

            auto isBasicFieldId = [](const std::string& id) {
                return id == "summary" ||
                       id == "assignee" ||
                       id == "priority" ||
                       id == "status" ||
                       id == "created" ||
                       id == "updated";
            };

            for (const auto& field : availableFields) {
                if (!ContainsCaseInsensitive(field.Id, d.fieldSearchBuf) &&
                    !ContainsCaseInsensitive(field.Name, d.fieldSearchBuf)) {
                    continue;
                }
                visibleFields.push_back(&field);
                if (field.IsCustom) {
                    customFields.push_back(&field);
                } else if (isBasicFieldId(field.Id)) {
                    basicFields.push_back(&field);
                } else {
                    systemFields.push_back(&field);
                }
            }

            const auto fieldSortLess = [](const JiraField* lhs, const JiraField* rhs) {
                if (!lhs || !rhs) return lhs != nullptr;
                if (lhs->Name != rhs->Name) return lhs->Name < rhs->Name;
                return lhs->Id < rhs->Id;
            };
            std::sort(systemFields.begin(), systemFields.end(), fieldSortLess);
            std::sort(customFields.begin(), customFields.end(), fieldSortLess);

            const auto syncSelectedFieldsBuffer = [&]() {
                const std::vector<std::string> updated = ToSortedVector(selectedFieldSet);
                const std::string csv = JoinCsv(updated);
                std::memset(d.selectedFieldsBuf, 0, sizeof(d.selectedFieldsBuf));
                std::strncpy(d.selectedFieldsBuf, csv.c_str(), sizeof(d.selectedFieldsBuf) - 1);
            };

            if (d.fieldCatalogLoading) {
                ImGui::BeginDisabled();
            }

            ImGui::TextDisabled("Selected: %zu", selectedFieldSet.size());
            ImGui::SameLine();
            ImGui::TextDisabled("Visible: %zu", visibleFields.size());
            ImGui::SameLine();
            if (ImGui::Button("Select All Visible")) {
                for (const JiraField* field : visibleFields) if (field) selectedFieldSet.insert(field->Id);
                syncSelectedFieldsBuffer();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Visible")) {
                for (const JiraField* field : visibleFields) if (field) selectedFieldSet.erase(field->Id);
                syncSelectedFieldsBuffer();
            }

            const auto renderFieldGroup = [&](const char* groupName, const std::vector<const JiraField*>& fields) {
                if (fields.empty()) return;
                const std::string label = std::string(groupName) + " (" + std::to_string(fields.size()) + ")";
                if (!ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
                for (const JiraField* field : fields) {
                    bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
                    const std::string checkboxId = "##ViewField_" + field->Id;
                    if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
                        if (checked) selectedFieldSet.insert(field->Id);
                        else selectedFieldSet.erase(field->Id);
                        syncSelectedFieldsBuffer();
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
                }
            };

            ImGui::BeginChild("ViewFieldsList", ImVec2(0, 220), true);
            if (availableFields.empty()) {
                ImGui::TextDisabled("No field catalog loaded yet.");
            } else if (visibleFields.empty()) {
                ImGui::TextDisabled("No fields match current search.");
            } else {
                // Basic Fields group: ID (always shown) + core Jira fields.
                {
                    const char* groupName = "Basic Fields";
                    const bool hasVisibleId = ContainsCaseInsensitive("id", d.fieldSearchBuf);
                    if (!basicFields.empty() || hasVisibleId) {
                        const std::size_t count = basicFields.size() + 1; // +1 for ID
                        const std::string label = std::string(groupName) + " (" + std::to_string(count) + ")";
                        if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            // ID: always selected, not toggleable.
                            bool idChecked = true;
                            ImGui::Checkbox("##ViewField_id", &idChecked);
                            ImGui::SameLine();
                            ImGui::Text("ID (id, always selected)");

                            auto renderBasicById = [&](const char* fid) {
                                auto it = std::find_if(
                                    basicFields.begin(),
                                    basicFields.end(),
                                    [&](const JiraField* f) { return f && f->Id == fid; });
                                if (it == basicFields.end() || !*it) return;
                                const JiraField* field = *it;
                                bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
                                const std::string checkboxId = "##ViewField_" + field->Id;
                                if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
                                    if (checked) selectedFieldSet.insert(field->Id);
                                    else selectedFieldSet.erase(field->Id);
                                    syncSelectedFieldsBuffer();
                                }
                                ImGui::SameLine();
                                ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
                            };

                            renderBasicById("summary");
                            renderBasicById("assignee");
                            renderBasicById("priority");
                            renderBasicById("status");
                            renderBasicById("created");
                            renderBasicById("updated");
                        }
                    }
                }

                renderFieldGroup("System Fields", systemFields);
                renderFieldGroup("Custom Fields", customFields);
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::Text("Column Order");
            ImGui::SameLine();
            ImGui::TextDisabled("(right-click a column to remove it from the view)");
            const std::vector<std::string> currentFields = ParseCsv(d.selectedFieldsBuf);
            std::unordered_set<std::string> validKeys = {"id"};
            for (const auto& f : currentFields) {
                validKeys.insert("field:" + f);
            }
            d.editingColumnOrder.erase(
                std::remove_if(d.editingColumnOrder.begin(), d.editingColumnOrder.end(),
                               [&](const std::string& key) { return validKeys.find(key) == validKeys.end(); }),
                d.editingColumnOrder.end());
            for (const auto& key : validKeys) {
                if (std::find(d.editingColumnOrder.begin(), d.editingColumnOrder.end(), key) == d.editingColumnOrder.end()) {
                    d.editingColumnOrder.push_back(key);
                }
            }

            ImGui::BeginChild("ColumnOrderList", ImVec2(0, 120), true);
            for (int i = 0; i < static_cast<int>(d.editingColumnOrder.size()); ++i) {
                const std::string& key = d.editingColumnOrder[static_cast<size_t>(i)];
                const bool selected = (d.selectedColumnOrderIndex == i);
                if (ImGui::Selectable(key.c_str(), selected)) {
                    d.selectedColumnOrderIndex = i;
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (key != "id" && ImGui::MenuItem("Remove column from view")) {
                        d.editingColumnOrder.erase(d.editingColumnOrder.begin() + i);
                        if (d.selectedColumnOrderIndex >= static_cast<int>(d.editingColumnOrder.size())) {
                            d.selectedColumnOrderIndex = static_cast<int>(d.editingColumnOrder.size()) - 1;
                        }
                        if (key.rfind("field:", 0) == 0) {
                            const std::string fieldId = key.substr(6);
                            std::vector<std::string> fields = ParseCsv(d.selectedFieldsBuf);
                            fields.erase(std::remove(fields.begin(), fields.end(), fieldId), fields.end());
                            const std::string csv = JoinCsv(fields);
                            std::memset(d.selectedFieldsBuf, 0, sizeof(d.selectedFieldsBuf));
                            std::strncpy(d.selectedFieldsBuf, csv.c_str(), sizeof(d.selectedFieldsBuf) - 1);
                            selectedFieldSet.clear();
                            for (const auto& fid : fields) {
                                selectedFieldSet.insert(fid);
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
            if (d.selectedColumnOrderIndex < 0 || d.selectedColumnOrderIndex >= static_cast<int>(d.editingColumnOrder.size())) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Move Up") && d.selectedColumnOrderIndex > 0) {
                std::swap(d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex)],
                          d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex - 1)]);
                --d.selectedColumnOrderIndex;
            }
            ImGui::SameLine();
            if (ImGui::Button("Move Down") &&
                d.selectedColumnOrderIndex >= 0 &&
                d.selectedColumnOrderIndex < static_cast<int>(d.editingColumnOrder.size()) - 1) {
                std::swap(d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex)],
                          d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex + 1)]);
                ++d.selectedColumnOrderIndex;
            }
            if (d.selectedColumnOrderIndex < 0 || d.selectedColumnOrderIndex >= static_cast<int>(d.editingColumnOrder.size())) {
                ImGui::EndDisabled();
            }

            if (d.fieldCatalogLoading) {
                ImGui::EndDisabled();
            }
        } else {
            ImGui::TextDisabled("No views available.");
        }

    ImGui::End();
}

void SmatchetUI::drawLuaAutomationWindow(AppController& app) {
    ImGui::Begin("Lua");
    if (ImGui::Button("Run Lua Automation")) {
        app.RunAutoScript("Automation.lua");
    }
    ImGui::TextDisabled("Runs Automation.lua for each active ticket.");
    ImGui::End();
}

void SmatchetUI::drawActiveProjectWindow(AppController& app) {
    auto& d = g_ui;
    ImGui::Begin("Smatchet - Active Project");

    ImGui::Separator();
    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (activeView) {
        ImGui::Text("View: %s", activeView->Name.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Refresh View")) {
            d.cfg.JqlQuery = activeView->Jql;
            d.cfg.SelectedFields = activeView->Fields;
            ConfigManager::Save(d.cfg);
            app.SyncWithBackend();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Views")) {
            d.showViewsDashboard = true;
        }
    } else {
        ImGui::TextDisabled("No active view.");
        ImGui::SameLine();
        if (ImGui::Button("Open Views")) {
            d.showViewsDashboard = true;
        }
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0, 1, 0, 1), "● MCP LIVE: 8080");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("External AI tools can now access Smatchet tickets via port 8080.");
    }

    ImGui::Separator();

    const auto& tickets = app.GetActiveTickets();

    JiraFieldCatalogIndex catalogIndex(app.GetAvailableJiraFields());
    const ViewDefinition* activeViewForGrid = ViewState.GetActiveView();
    const std::vector<TicketGridColumn> columns =
        activeViewForGrid ? TicketGridColumnsBuilder::Build(*activeViewForGrid, catalogIndex)
                   : std::vector<TicketGridColumn>();
    TicketEditService ticketEditService(app);

    if (!d.gridEditError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", d.gridEditError.c_str());
        ImGui::PopStyleColor();
    } else if (!d.gridEditSuccess.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
        ImGui::TextWrapped("%s", d.gridEditSuccess.c_str());
        ImGui::PopStyleColor();
    }

    std::vector<PendingFieldEdit> pendingEdits;
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SortMulti |
        ImGuiTableFlags_NoSavedSettings;

    if (!columns.empty() && ImGui::BeginTable("TicketGrid", static_cast<int>(columns.size()), tableFlags)) {
        for (const auto& column : columns) {
            const float persistedWidth = activeViewForGrid
                ? (activeViewForGrid->ColumnWidths.count(column.Key) ? activeViewForGrid->ColumnWidths.at(column.Key) : 0.0f)
                : 0.0f;
            const float defaultWidth = (column.ColumnKind == TicketGridColumn::Kind::Id) ? 90.0f : 180.0f;
            const float width = persistedWidth > 0.0f ? persistedWidth : defaultWidth;
            if (column.ColumnKind == TicketGridColumn::Kind::Id) {
                ImGui::TableSetupColumn(column.Label.c_str(), ImGuiTableColumnFlags_WidthFixed, width);
            } else {
                ImGui::TableSetupColumn(column.Label.c_str(), ImGuiTableColumnFlags_WidthFixed, width);
            }
        }
        ImGui::TableHeadersRow();

        // When the active view changes, the table still holds the previous view's sort; clear and apply
        // the new view's sort so we don't persist the old sort into the new view.
        const bool viewChanged = activeViewForGrid && (activeViewForGrid->Id != d.lastGridActiveViewId);
        if (viewChanged) {
            d.lastGridActiveViewId = activeViewForGrid->Id;
        }
        if (!activeViewForGrid) {
            d.lastGridActiveViewId.clear();
        }

        // Apply persisted sort from view when ImGui has no sort yet, or when we just switched view.
        if (activeViewForGrid && !activeViewForGrid->SortSpecs.empty()) {
            ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
            if (specs && (specs->SpecsCount == 0 || viewChanged)) {
                for (size_t i = 0; i < activeViewForGrid->SortSpecs.size(); ++i) {
                    const ViewSortSpec& vs = activeViewForGrid->SortSpecs[i];
                    if (vs.Direction == 0) continue;
                    int colIndex = -1;
                    for (size_t c = 0; c < columns.size(); ++c) {
                        if (columns[c].Key == vs.ColumnKey) { colIndex = static_cast<int>(c); break; }
                    }
                    if (colIndex >= 0) {
                        ImGui::TableSetColumnSortDirection(colIndex, static_cast<ImGuiSortDirection>(vs.Direction), (i > 0));
                    }
                }
            }
        }

        ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        std::vector<size_t> sortedIndices;
        if (sortSpecs && sortSpecs->SpecsCount > 0 && sortSpecs->Specs != nullptr) {
            sortedIndices.resize(tickets.size());
            for (size_t i = 0; i < tickets.size(); ++i) sortedIndices[i] = i;
            const std::vector<CachedTicket>* ticketsPtr = &tickets;
            const std::vector<TicketGridColumn>* columnsPtr = &columns;
            const JiraFieldCatalogIndex* catalogPtr = &catalogIndex;
            std::stable_sort(sortedIndices.begin(), sortedIndices.end(),
                [ticketsPtr, columnsPtr, catalogPtr, sortSpecs](size_t ia, size_t ib) {
                    const auto& tix = *ticketsPtr;
                    const auto& cols = *columnsPtr;
                    for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
                        const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[s];
                        const int colIndex = spec.ColumnIndex;
                        if (colIndex < 0 || colIndex >= static_cast<int>(cols.size())) continue;
                        const TicketGridColumn& col = cols[static_cast<size_t>(colIndex)];
                        const int dir = (spec.SortDirection == ImGuiSortDirection_Ascending) ? 1 : -1;
                        if (col.ColumnKind == TicketGridColumn::Kind::Id) {
                            const bool less = CompareIssueKeyNatural(tix[ia].id, tix[ib].id);
                            if (less) return dir > 0;
                            if (!CompareIssueKeyNatural(tix[ib].id, tix[ia].id)) continue;
                            return dir < 0;
                        }
                        const std::string aVal = tix[ia].GetFieldValue(col.FieldId);
                        const std::string bVal = tix[ib].GetFieldValue(col.FieldId);
                        const JiraField* fieldMeta = catalogPtr->Find(col.FieldId);
                        const int cmp = CompareFieldValuesForSort(col.FieldId, fieldMeta, aVal, bVal, spec.SortDirection);
                        if (cmp != 0) return (cmp * dir) < 0;
                    }
                    return ia < ib;
                });
        }

        const std::vector<size_t>* indicesToUse = sortedIndices.empty() ? nullptr : &sortedIndices;
        for (size_t r = 0; r < tickets.size(); ++r) {
            const size_t ticketIndex = indicesToUse ? (*indicesToUse)[r] : r;
            const CachedTicket& ticket = tickets[ticketIndex];
            bool isSelected = (d.gridState.SelectedId == ticket.id);
            ImGui::TableNextRow();

            for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
                const auto& column = columns[static_cast<size_t>(colIndex)];
                ImGui::TableSetColumnIndex(colIndex);

                if (column.ColumnKind == TicketGridColumn::Kind::Id) {
                    if (ImGui::Selectable(ticket.id.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        d.gridState.SelectRow(ticket.id);
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            const std::string url = BuildJiraBrowseUrl(d.cfg, ticket.id);
                            OpenUrlInDefaultBrowser(url);
                        }
                    }
                    continue;
                }

                const std::string currentValue = ticket.GetFieldValue(column.FieldId);
                const JiraField* fieldMeta = catalogIndex.Find(column.FieldId);
                const float cellStartY = ImGui::GetCursorScreenPos().y;
                const float cellRightX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                const float valueAvailWidth = cellRightX - ImGui::GetCursorScreenPos().x;
                const std::string cellKey = BuildCellKey(ticket.id, column.FieldId);
                const auto feedbackIt = d.cellFeedbackByKey.find(cellKey);
                const bool isSavingThisCell =
                    feedbackIt != d.cellFeedbackByKey.end() &&
                    feedbackIt->second.State == CellWriteState::Saving;

                bool showBadge = false;
                ImVec4 badgeColor(1.0f, 1.0f, 1.0f, 1.0f);
                std::string badgeText;
                std::string badgeTooltip;
                if (feedbackIt != d.cellFeedbackByKey.end() &&
                    feedbackIt->second.State == CellWriteState::Error &&
                    !feedbackIt->second.Message.empty()) {
                    showBadge = true;
                    badgeColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                    badgeText = "!";
                    badgeTooltip = feedbackIt->second.Message;
                } else if (feedbackIt != d.cellFeedbackByKey.end() &&
                           feedbackIt->second.State == CellWriteState::Success) {
                    showBadge = true;
                    badgeColor = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
                    badgeText = "OK";
                    badgeTooltip = "Saved";
                }

                if (isSavingThisCell) {
                    showBadge = true;
                    badgeColor = ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
                    badgeText = "...";
                    badgeTooltip = "Saving...";
                    RenderClippedFieldText(currentValue, valueAvailWidth, d.cfg.EnableFieldOverflowTooltips, true);
                } else {
                    TicketFieldEditor::RenderFieldCell(
                        ticket,
                        fieldMeta,
                        currentValue,
                        valueAvailWidth,
                        d.cfg.EnableFieldOverflowTooltips,
                        d.gridState,
                        pendingEdits);
                }

                if (showBadge) {
                    const ImVec2 textSize = ImGui::CalcTextSize(badgeText.c_str());
                    const float badgeX = std::max(ImGui::GetCursorScreenPos().x, cellRightX - textSize.x);
                    ImGui::SetCursorScreenPos(ImVec2(badgeX, cellStartY));
                    ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
                    ImGui::TextUnformatted(badgeText.c_str());
                    ImGui::PopStyleColor();
                    if (!badgeTooltip.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", badgeTooltip.c_str());
                    }
                }
            }
        }

        // Persist column widths and sort specs back to the active view.
        if (activeViewForGrid) {
            ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
            if (mutableActive) {
                bool metaChanged = false;
                ImGuiTable* table = ImGui::GetCurrentTable();
                if (table) {
                    // Persist column widths only; column order is controlled via the Views dashboard.
                    for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
                        const std::string& key = columns[static_cast<size_t>(i)].Key;
                        const float width = (i < table->ColumnsCount)
                            ? table->Columns[i].WidthGiven
                            : 0.0f;
                        const auto oldIt = mutableActive->ColumnWidths.find(key);
                        const float oldWidth = (oldIt == mutableActive->ColumnWidths.end()) ? 0.0f : oldIt->second;
                        if (std::abs(oldWidth - width) > 0.5f) {
                            mutableActive->ColumnWidths[key] = width;
                            metaChanged = true;
                        }
                    }
                }
                // Re-fetch sort specs from the table right before persisting so we use current state.
                ImGuiTableSortSpecs* currentSortSpecs = ImGui::TableGetSortSpecs();
                if (currentSortSpecs && currentSortSpecs->SpecsCount > 0 && currentSortSpecs->Specs != nullptr) {
                    std::vector<ViewSortSpec> newSortSpecs;
                    for (int s = 0; s < currentSortSpecs->SpecsCount; ++s) {
                        const int colIndex = currentSortSpecs->Specs[s].ColumnIndex;
                        if (colIndex >= 0 && colIndex < static_cast<int>(columns.size()) &&
                            currentSortSpecs->Specs[s].SortDirection != ImGuiSortDirection_None) {
                            ViewSortSpec vs;
                            vs.ColumnKey = columns[static_cast<size_t>(colIndex)].Key;
                            vs.Direction = static_cast<int>(currentSortSpecs->Specs[s].SortDirection);
                            newSortSpecs.push_back(vs);
                        }
                    }
                    if (newSortSpecs != mutableActive->SortSpecs) {
                        mutableActive->SortSpecs = std::move(newSortSpecs);
                        metaChanged = true;
                    }
                } else {
                    // No active sort in the table; clear stored sort so it doesn't persist stale state.
                    if (!mutableActive->SortSpecs.empty()) {
                        mutableActive->SortSpecs.clear();
                        metaChanged = true;
                    }
                }
                if (metaChanged) {
                    ViewState.Save();
                }
            }
        }
        ImGui::EndTable();
    }

    // Keep queued edits latest-per-cell (drop older queued item for same cell).
    for (const auto& edit : pendingEdits) {
        const std::string editKey = BuildCellKey(edit.IssueId, edit.Field.Id);
        for (auto it = d.queuedFieldEdits.begin(); it != d.queuedFieldEdits.end();) {
            if (BuildCellKey(it->IssueId, it->Field.Id) == editKey) {
                it = d.queuedFieldEdits.erase(it);
            } else {
                ++it;
            }
        }
        d.queuedFieldEdits.push_back(edit);
    }

    if (!d.hasInFlightEdit && !d.queuedFieldEdits.empty()) {
        d.inFlightEdit = d.queuedFieldEdits.front();
        d.queuedFieldEdits.pop_front();
        d.hasInFlightEdit = true;
        d.inFlightDelayFrames = 1;
        CellWriteFeedback feedback;
        feedback.State = CellWriteState::Saving;
        feedback.Message = "Saving to Jira...";
        feedback.FramesRemaining = 0;
        d.cellFeedbackByKey[BuildCellKey(d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id)] = feedback;
    }

    if (d.hasInFlightEdit) {
        if (d.inFlightDelayFrames > 0) {
            --d.inFlightDelayFrames;
        } else {
            std::string commitError;
            const std::string editKey = BuildCellKey(d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id);
            if (!ticketEditService.Commit(d.inFlightEdit, commitError)) {
                d.gridEditError = commitError.empty()
                    ? std::string("Failed to save Jira field update.")
                    : commitError;
                d.gridEditSuccess.clear();
                CellWriteFeedback feedback;
                feedback.State = CellWriteState::Error;
                feedback.Message = d.gridEditError;
                feedback.FramesRemaining = 0;
                d.cellFeedbackByKey[editKey] = feedback;
            } else {
                d.gridEditSuccess = "Field update saved to Jira.";
                d.gridEditError.clear();
                CellWriteFeedback feedback;
                feedback.State = CellWriteState::Success;
                feedback.Message = "Saved";
                feedback.FramesRemaining = 180;
                d.cellFeedbackByKey[editKey] = feedback;
            }
            d.hasInFlightEdit = false;
        }
    }

    for (auto it = d.cellFeedbackByKey.begin(); it != d.cellFeedbackByKey.end();) {
        if (it->second.State == CellWriteState::Success && it->second.FramesRemaining > 0) {
            --it->second.FramesRemaining;
        }

        if (it->second.State == CellWriteState::Success && it->second.FramesRemaining <= 0) {
            it = d.cellFeedbackByKey.erase(it);
        } else {
            ++it;
        }
    }

    if (!pendingEdits.empty()) {
        d.gridEditError.clear();
    }
    ImGui::End();
}

void SmatchetUI::drawAIAssistantWindow(AppController& app) {
    auto& d = g_ui;
    ImGui::Begin("AI Assistant");

    if (d.gridState.SelectedId.empty()) {
        ImGui::TextDisabled("Select a ticket to see AI insights.");
    } else {
        // Find the selected ticket data in the app's cache
        const auto& tickets = app.GetActiveTickets();
        auto it = std::find_if(tickets.begin(), tickets.end(),
                               [&](const CachedTicket& t){ return t.id == d.gridState.SelectedId; });

        if (it != tickets.end()) {
            const std::string summary = it->GetFieldValue("summary");
            ImGui::Text("Analyzing: %s", it->id.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", summary.empty() ? "<no summary field selected>" : summary.c_str());
            if (!it->fieldValues.empty()) {
                ImGui::Spacing();
                ImGui::Text("Selected Jira Fields");
                ImGui::Separator();
                for (const auto& kv : it->fieldValues) {
                    const std::string label = kv.first + ": ";
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::SameLine();
                    const float valueAvail = ImGui::GetContentRegionAvail().x;
                    RenderClippedFieldText(kv.second, valueAvail, d.cfg.EnableFieldOverflowTooltips, false);
                }
            }
            ImGui::Spacing();

            if (ImGui::Button("Generate Action Plan") && !d.aiIsThinking) {
                d.aiIsThinking = true;
                std::string aiKey = "YOUR_API_KEY_HERE";

                auto result = AiController::AnalyzeTicket(it->id, summary, aiKey);
                d.aiResponse = result.Response;
                d.aiIsThinking = false;
            }

            if (d.aiIsThinking) ImGui::Text("AI is thinking...");

            if (!d.aiResponse.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));
                ImGui::TextWrapped("%s", d.aiResponse.c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
}

void SmatchetUI::drawLogWindow() {
    auto& d = g_ui;
    ImGui::Begin("Log");

    if (ImGui::Button("Clear Log")) {
        Logger::Instance().Clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(showing application log messages)");

    ImGui::Separator();

    const auto entries = Logger::Instance().GetEntriesSnapshot();

    {
        std::string aggregated;
        aggregated.reserve(entries.size() * 64);

        for (const auto& e : entries) {
            const char* levelLabel = "";
            switch (e.level) {
                case LogLevel::Trace: levelLabel = "[TRACE] "; break;
                case LogLevel::Debug: levelLabel = "[DEBUG] "; break;
                case LogLevel::Info:  levelLabel = "[INFO ] "; break;
                case LogLevel::Warn:  levelLabel = "[WARN ] "; break;
                case LogLevel::Error: levelLabel = "[ERROR] "; break;
            }
            aggregated += levelLabel;
            aggregated += e.message;
            aggregated += '\n';
        }

        d.logBuffer.assign(aggregated.begin(), aggregated.end());
        d.logBuffer.push_back('\0');
    }

    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::InputTextMultiline(
        "##LogText",
        d.logBuffer.data(),
        d.logBuffer.size(),
        ImVec2(-FLT_MIN, -FLT_MIN),
        ImGuiInputTextFlags_ReadOnly
    );
    ImGui::EndChild();
    ImGui::End();
}