#include "Ui/SmatchetBulkFieldEditPopup.h"

// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=the popup calls seven AppController accessors (component options, connectivity, pane cache key, pane target latch, ticket snapshot) that no IApp* facet exposes together yet; owner=grid-engine; revisit=when the grid popups move onto a narrow IAppGrid facet per ADR-0020)
#include "AppController.h"
#include "ComponentOptionsPickPure.h"
#include "DataFreshnessCue.h"
#include "FilterDropdown.h"
#include "Logger.h"
#include "RecentSelections.h"
#include "SmatchetGridUiSupport.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "StringUtil.h"
#include "Tracker/ProjectResolver.h"
#include "Tracker/TrackerFieldValueUtils.h"
#include "Tracker/TrackerLabelsPure.h"
#include "TrackerOptionDropdown.h"
#include "Ui/BulkFieldEditPure.h"

#include "imgui.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

using smatchet::grid::bulk::BulkFieldEditJob;
using smatchet::grid::bulk::BulkFieldKind;
using smatchet::grid::bulk::BulkMode;
using smatchet::grid::bulk::BulkRow;
using smatchet::grid::bulk::BulkRowState;

constexpr const char* kPickSuffix = "###bulk_edit_pick";
constexpr const char* kConfirmId = "###bulk_edit_confirm";
constexpr int kMaxSkipLinesShown = 8;

// Everything one frame of the popup needs; built on the stack, so nothing is kept between frames.
struct PopupEnv {
    AppController& App;
    UiDrawSession& D;
    GridPane& Pane;
    BulkEditPopupRequest& Req;
    bool ReadOnly;
};

// The option list the picker offers. Components are scoped to the clicked row's own Jira project (the same rule as the
// grid's components cell, PickRowComponents in TicketFieldEditor.cpp); every other field uses the catalog's list.
struct OptionList {
    ProjectComponentsLookup Holder; // keeps the project list alive
    const std::vector<TrackerFieldOption>* Options = nullptr;
    smatchet::offline::DataFreshness Freshness = smatchet::offline::DataFreshness::Fresh;
};

bool IsComponentsField(const TrackerField& field) { return ToLowerAsciiCopy(field.Id) == "components"; }

OptionList ResolveOptionList(AppController& app, const BulkEditPopupRequest& req) {
    OptionList list;
    list.Options = &req.Field.AllowedValueOptions;
    if (!IsComponentsField(req.Field)) {
        return list;
    }
    const std::string projectKey = smatchet::ExtractIssueKeyPrefix(req.ClickedIssueId);
    list.Holder = app.GetComponentOptionsForProject(projectKey);
    if (list.Holder.freshness != smatchet::offline::DataFreshness::Fresh &&
        app.EnsureProjectComponentsLoaded(projectKey)) {
        list.Holder = app.GetComponentOptionsForProject(projectKey); // report the fetch this frame, not the next
    }
    // req.Field's options are the catalog's as it was when the popup opened: trust them for this row only when that
    // catalog was fetched for the row's project (the scope captured with the field), never by the live catalog.
    const bool catalogIsRowProject =
        !list.Holder.options && smatchet::components::CatalogIsRowProject(req.FieldCatalogProjectKey, projectKey);
    const smatchet::components::ComponentOptionsPick pick = smatchet::components::PickComponentOptions(
        list.Holder, catalogIsRowProject, req.Field.AllowedValueOptions, app.GetLastTrackerConnectivityState());
    list.Options = pick.Options;
    list.Freshness = pick.Freshness;
    return list;
}

// The field as the job must see it: components carry the clicked row's project list, so each row's before-value
// resolves against it. A copy: made on each draw of the confirm step and once on apply, so only while the modal is
// open.
TrackerField EffectiveField(AppController& app, const BulkEditPopupRequest& req) {
    TrackerField field = req.Field;
    if (IsComponentsField(field)) {
        // Always the row's resolved list, even when it is empty: keeping req.Field's catalog options would let the
        // before-values and the entry check use another project's components (Ui AGENTS.md, Pillar 6).
        const OptionList list = ResolveOptionList(app, req);
        field.AllowedValueOptions = *list.Options;
    }
    return field;
}

std::string FieldDisplayName(const TrackerField& field) { return field.Name.empty() ? field.Id : field.Name; }

std::string ValuesLabel(const TrackerField& field, const std::vector<std::string>& values) {
    if (values.empty()) {
        return SmatchetLocalization::T("grid.bulk.value_none", "(empty)");
    }
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += TrackerFieldValueUtils::ResolveOptionLabel(field, value);
    }
    return out;
}

// "N rows · mixed (k values)" or "N rows · <value>" when every row holds the same value.
std::string HeaderText(const BulkFieldEditJob& preview) {
    const int distinct = smatchet::grid::bulk::DistinctBeforeValueCount(preview);
    std::string value;
    if (distinct > 1) {
        value = SmatchetLocalization::Format("grid.bulk.mixed", "mixed (%d values)", distinct);
    } else {
        value = smatchet::grid::bulk::MixedValueLabel(preview);
        if (value.empty()) {
            value = SmatchetLocalization::T("grid.bulk.value_none", "(empty)");
        }
    }
    return SmatchetLocalization::Format("grid.bulk.header", "%d rows \xC2\xB7 %s",
                                        static_cast<int>(preview.Rows.size()), value.c_str());
}

// The rows the pre-flight will skip, each with its reason, so the user sees them before anything is sent.
void DrawSkippedRows(const BulkFieldEditJob& job) {
    const smatchet::grid::bulk::BulkSummary summary = smatchet::grid::bulk::Summarize(job);
    if (summary.Skipped == 0) {
        return;
    }
    const std::string title =
        SmatchetLocalization::Format("grid.bulk.skip_summary", "%d rows will be skipped", summary.Skipped);
    if (!ImGui::TreeNode("##bulk_skipped", "%s", title.c_str())) {
        return;
    }
    int shown = 0;
    for (const BulkRow& row : job.Rows) {
        if (row.State != BulkRowState::Skipped) {
            continue;
        }
        if (shown == kMaxSkipLinesShown) {
            ImGui::TextDisabled("%s", SmatchetLocalization::Format("grid.bulk.skip_more", "+ %d more",
                                                                   summary.Skipped - kMaxSkipLinesShown));
            break;
        }
        ImGui::BulletText("%s: %s", row.IssueId.c_str(), row.Message.c_str());
        ++shown;
    }
    ImGui::TreePop();
}

// Every label seen in the pane's tickets, once, when the popup opens (the labels editor does the same per cell).
void CollectLabelSuggestions(AppController& app, BulkEditPopupRequest& req) {
    req.LabelSuggestions.clear();
    const PendingActionTarget target = app.LatchPendingActionTargetForPane(req.PaneId);
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.TicketsSnapshotForTarget(target);
    if (!tickets) {
        return;
    }
    std::vector<std::string> labels;
    for (const CachedTicket& ticket : *tickets) {
        for (const std::string& part : TrackerLabelsPure::ParseCsv(ticket.GetFieldValue(req.Field.Id))) {
            const std::string trimmed = TrimCopy(part);
            if (!trimmed.empty()) {
                labels.push_back(trimmed);
            }
        }
    }
    req.LabelSuggestions = TrackerLabelsPure::SortAndUniqueLabels(std::move(labels));
}

// The popup was just requested: take the snapshot the header and the skip list read.
void PrepareOpen(PopupEnv& e) {
    BulkEditPopupRequest& req = e.Req;
    req.OpenPending = false;
    req.CurrentStage = BulkEditPopupRequest::Stage::Pick;
    req.Mode = BulkMode::Replace;
    req.Picked.clear();
    req.PendingValues.clear();
    req.Clear = false;
    // The preview's before-values resolve against the row's own list too (EffectiveField), as the apply does.
    req.Preview = PrepareBulkFieldEditJob(e.App, req.PaneId, EffectiveField(e.App, req), req.IssueIds,
                                          std::vector<std::string>(), BulkMode::Replace, nullptr);
    if (smatchet::grid::bulk::ClassifyBulkField(req.Field) == BulkFieldKind::Labels) {
        CollectLabelSuggestions(e.App, req);
    }
    ImGui::OpenPopup(kPickSuffix);
}

void DrawModeToggle(BulkEditPopupRequest& req) {
    int mode = static_cast<int>(req.Mode);
    ImGui::RadioButton(SmatchetLocalization::Label("grid.bulk.mode_replace", "Replace", "bulk_mode_replace"), &mode,
                       static_cast<int>(BulkMode::Replace));
    ImGui::SameLine();
    ImGui::RadioButton(SmatchetLocalization::Label("grid.bulk.mode_add", "Add", "bulk_mode_add"), &mode,
                       static_cast<int>(BulkMode::Add));
    ImGui::SameLine();
    ImGui::RadioButton(SmatchetLocalization::Label("grid.bulk.mode_remove", "Remove", "bulk_mode_remove"), &mode,
                       static_cast<int>(BulkMode::Remove));
    req.Mode = static_cast<BulkMode>(mode);
}

// The user chose the values: send now, or ask first when the selection is large (the confirm popup then opens on the
// next draw). The caller closes the picker.
void Proceed(PopupEnv& e, const std::vector<std::string>& values) {
    BulkEditPopupRequest& req = e.Req;
    const TrackerField field = EffectiveField(e.App, req);
    const BulkMode mode = req.Mode;
    if (smatchet::grid::bulk::NeedsConfirm(static_cast<int>(req.Preview.Rows.size()))) {
        req.PendingValues = values;
        // Re-run the pre-flight with the values, so the confirm text knows how many rows will really change.
        std::vector<std::vector<std::string>> rowValues;
        req.Preview = PrepareBulkFieldEditJob(e.App, req.PaneId, field, req.IssueIds, values, mode, &rowValues);
        req.CurrentStage = BulkEditPopupRequest::Stage::Confirm;
        req.ConfirmPending = true;
        return;
    }
    StartBulkFieldEdit(e.App, e.D, req.PaneId, field, req.IssueIds, values, mode, e.ReadOnly);
    req.Active = false;
}

// The option rows of the picker, as the shared dropdown reads them. Pointers into `list`, `req` and `common`.
std::vector<smatchet::dropdown::Item> BuildItems(const BulkEditPopupRequest& req, const OptionList& list,
                                                 BulkFieldKind kind, const std::string& commonBefore) {
    std::vector<smatchet::dropdown::Item> items;
    const auto picked = [&req](const char* id) {
        return std::find(req.Picked.begin(), req.Picked.end(), id) != req.Picked.end();
    };
    if (kind == BulkFieldKind::Labels) {
        items.reserve(req.LabelSuggestions.size());
        for (const std::string& label : req.LabelSuggestions) {
            smatchet::dropdown::Item item;
            item.Id = label.c_str();
            item.Label = label.c_str();
            item.Checked = picked(item.Id);
            items.push_back(item);
        }
        return items;
    }
    items.reserve(list.Options->size());
    for (const TrackerFieldOption& option : *list.Options) {
        smatchet::dropdown::Item item = smatchet::dropdown::ItemForOption(option);
        item.Disabled = option.Disabled;
        if (kind == BulkFieldKind::MultiChoice) {
            item.Tooltip = item.Secondary;
            item.Checked = picked(item.Id);
        } else {
            item.Checked = !commonBefore.empty() && commonBefore == item.Id;
        }
        items.push_back(item);
    }
    return items;
}

// What a pick did, for a tick-box picker: toggles the ticked set; a clear empties it.
void ApplyMultiEvent(BulkEditPopupRequest& req, const smatchet::dropdown::Event& ev) {
    if (ev.Kind == smatchet::dropdown::EventKind::Cleared) {
        req.Picked.clear();
        req.Clear = true;
        return;
    }
    if (ev.Kind != smatchet::dropdown::EventKind::Picked && ev.Kind != smatchet::dropdown::EventKind::Created) {
        return;
    }
    const auto it = std::find(req.Picked.begin(), req.Picked.end(), ev.Id);
    if (it != req.Picked.end()) {
        if (ev.Kind == smatchet::dropdown::EventKind::Picked) {
            req.Picked.erase(it);
        }
    } else {
        req.Picked.push_back(ev.Id);
        // A label typed in the filter is not among the suggestions: add it, or the picker would never show it ticked.
        if (ev.Kind == smatchet::dropdown::EventKind::Created &&
            std::find(req.LabelSuggestions.begin(), req.LabelSuggestions.end(), ev.Id) == req.LabelSuggestions.end()) {
            req.LabelSuggestions.push_back(ev.Id);
            req.LabelSuggestions = TrackerLabelsPure::SortAndUniqueLabels(std::move(req.LabelSuggestions));
        }
    }
    req.Clear = false;
}

// Apply / Cancel row of the multi-value picker. Apply needs a pick, or Replace with an explicit clear.
void DrawMultiButtons(PopupEnv& e) {
    BulkEditPopupRequest& req = e.Req;
    const bool canApply = !req.Picked.empty() || (req.Mode == BulkMode::Replace && req.Clear);
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button(SmatchetLocalization::Label("grid.bulk.apply", "Apply", "bulk_pick_apply"))) {
        Proceed(e, req.Picked);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::Label("grid.bulk.cancel", "Cancel", "bulk_pick_cancel"))) {
        req.Active = false;
        ImGui::CloseCurrentPopup();
    }
}

// The value picker: header, skip list, the mode toggle for multi-value fields, then the shared dropdown body. A
// single-select pick closes the popup (the dropdown does that itself), a tick-box pick leaves it open.
void DrawPickBody(PopupEnv& e) {
    BulkEditPopupRequest& req = e.Req;
    const BulkFieldKind kind = smatchet::grid::bulk::ClassifyBulkField(req.Field);
    const bool multi = smatchet::grid::bulk::IsMultiValueKind(kind);

    ImGui::TextUnformatted(HeaderText(req.Preview).c_str());
    DrawSkippedRows(req.Preview);
    if (multi) {
        DrawModeToggle(req);
    }
    ImGui::Separator();

    const OptionList list = ResolveOptionList(e.App, req);
    if (list.Freshness != smatchet::offline::DataFreshness::Fresh) {
        DataFreshnessCue::Draw(list.Freshness);
        ImGui::Separator();
    }
    std::string commonBefore;
    if (!multi && smatchet::grid::bulk::DistinctBeforeValueCount(req.Preview) == 1) {
        for (const BulkRow& row : req.Preview.Rows) {
            if (row.State != BulkRowState::Skipped && !row.BeforeValues.empty()) {
                commonBefore = row.BeforeValues.front();
                break;
            }
        }
    }
    const std::vector<smatchet::dropdown::Item> items = BuildItems(req, list, kind, commonBefore);

    // The pane's own cache key, never the focused pane's: the recent picks are per tracker instance, and a split over
    // two trackers shows rows of the pane that was clicked.
    const std::string cacheKey = !req.PaneCacheKey.empty() ? req.PaneCacheKey : e.App.PaneCacheBackendKey(req.PaneId);
    const std::string recentsScope = smatchet::recents::MakeScopeKey(cacheKey, req.Field.Id);
    smatchet::dropdown::Options options =
        multi ? smatchet::dropdown::TickBoxes(recentsScope, SmatchetLocalization::T("field.clear_all", "<clear all>"))
              : smatchet::dropdown::SingleSelect(recentsScope);
    if (!multi && req.Field.Id != "status") {
        options.ClearLabel = SmatchetLocalization::T("field.clear", "<clear>"); // a status cannot be emptied
    }
    options.AllowCreate = kind == BulkFieldKind::Labels;
    if (kind == BulkFieldKind::Labels) {
        options.EmptyText = SmatchetLocalization::T("field.no_labels", "(no labels discovered yet)");
    }
    const smatchet::dropdown::Event ev = smatchet::dropdown::DrawBody(items, options);

    if (multi) {
        ApplyMultiEvent(req, ev);
        ImGui::Separator();
        DrawMultiButtons(e);
        return;
    }
    if (ev.Kind == smatchet::dropdown::EventKind::Picked) {
        Proceed(e, std::vector<std::string>{ev.Id});
        ImGui::CloseCurrentPopup(); // the body closes on a pick; a clear may not, and the request is done either way
    } else if (ev.Kind == smatchet::dropdown::EventKind::Cleared) {
        Proceed(e, std::vector<std::string>());
        ImGui::CloseCurrentPopup();
    }
}

void DrawConfirmBody(PopupEnv& e) {
    BulkEditPopupRequest& req = e.Req;
    const smatchet::grid::bulk::BulkSummary summary = smatchet::grid::bulk::Summarize(req.Preview);
    const std::string fieldName = FieldDisplayName(req.Field);
    TrackerField effective; // components name their values from the clicked row's project list
    const TrackerField* shown = &req.Field;
    if (IsComponentsField(req.Field)) {
        effective = EffectiveField(e.App, req);
        shown = &effective;
    }
    ImGui::TextWrapped("%s",
                       SmatchetLocalization::Format("grid.bulk.confirm_body", "Apply to %d rows? %d will be skipped.",
                                                    summary.Remaining, summary.Skipped));
    ImGui::TextWrapped("%s", SmatchetLocalization::Format("grid.bulk.confirm_value", "Set %s to: %s", fieldName.c_str(),
                                                          ValuesLabel(*shown, req.PendingValues).c_str()));
    DrawSkippedRows(req.Preview);
    ImGui::Separator();
    if (ImGui::Button(SmatchetLocalization::Label("grid.bulk.apply", "Apply", "bulk_confirm_apply"))) {
        StartBulkFieldEdit(e.App, e.D, req.PaneId, EffectiveField(e.App, req), req.IssueIds, req.PendingValues,
                           req.Mode, e.ReadOnly);
        req.Active = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus(); // Enter applies: the default-focused button of the confirm step
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::Label("grid.bulk.cancel", "Cancel", "bulk_confirm_cancel"))) {
        req.Active = false;
        ImGui::CloseCurrentPopup();
    }
}

void DrawPickStage(PopupEnv& e) {
    BulkEditPopupRequest& req = e.Req;
    if (req.OpenPending) {
        PrepareOpen(e);
    }
    const std::string fieldName = FieldDisplayName(req.Field);
    const std::string title =
        std::string(SmatchetLocalization::Format("grid.bulk.title", "Set %s for %d rows", fieldName.c_str(),
                                                 static_cast<int>(req.Preview.Rows.size()))) +
        kPickSuffix;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        DrawPickBody(e);
        ImGui::EndPopup();
    } else if (!req.OpenPending && !req.ConfirmPending) {
        req.Active = false; // closed with Esc or the X
    }
    // A pick that needs the confirm step closed the picker above; the confirm popup opens on the next draw.
}

void DrawConfirmStage(PopupEnv& e) {
    BulkEditPopupRequest& req = e.Req;
    if (req.ConfirmPending) {
        req.ConfirmPending = false;
        ImGui::OpenPopup(kConfirmId);
    }
    const std::string title =
        std::string(SmatchetLocalization::T("grid.bulk.confirm_title", "Confirm bulk edit")) + kConfirmId;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::BeginPopupModal(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        DrawConfirmBody(e);
        ImGui::EndPopup();
    } else {
        req.Active = false; // closed with Esc or the X
    }
}

} // namespace

void DrawBulkFieldEditPopup(AppController& app, UiDrawSession& d, GridPane& pane, bool readOnlyMode) {
    BulkEditPopupRequest& req = d.bulkEditPopupRequest;
    if (!req.Active || req.PaneId != pane.id) {
        return;
    }
    PopupEnv env{app, d, pane, req, readOnlyMode};
    if (req.CurrentStage == BulkEditPopupRequest::Stage::Pick) {
        DrawPickStage(env);
    } else {
        DrawConfirmStage(env);
    }
}
