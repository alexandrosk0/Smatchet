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
#include "LearnedWorkflowPure.h"
#include "Tracker/ProjectResolver.h"
#include "Tracker/TrackerFieldValueUtils.h"
#include "Tracker/TrackerLabelsPure.h"
#include "TicketFieldEditor.h"
#include "TrackerOptionDropdown.h"
#include "Ui/BulkFieldEditPure.h"

#include "imgui.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
// grid's components cell: PickProjectComponents); every other field uses the catalog's list.
struct OptionList {
    ProjectComponentsLookup Holder;                // keeps the project list alive
    std::vector<TrackerFieldOption> StatusOptions; // the status list: the moves every row allows
    // Never points at StatusOptions: the struct is returned by value, so a pointer to its own member would dangle.
    const std::vector<TrackerFieldOption>* Options = nullptr;
    smatchet::offline::DataFreshness Freshness = smatchet::offline::DataFreshness::Fresh;
    bool IsTransitionList = false; // the list is StatusOptions, built from the rows' transitions
    smatchet::statuscombo::StatusOptionsSource StatusFrom = smatchet::statuscombo::StatusOptionsSource::Live;

    const std::vector<TrackerFieldOption>& Opts() const { return IsTransitionList ? StatusOptions : *Options; }
};

bool IsComponentsField(const TrackerField& field) { return ToLowerAsciiCopy(field.Id) == "components"; }

bool IsStatusField(const TrackerField& field) { return field.Id == "status"; }

// Live transitions fetches the status picker keeps in flight at once (one thread each): a selection of hundreds of rows
// must not send a request per row. Rows that are not live yet take turns (FetchCursor), so a row held back by the
// failure backoff never starves the others.
constexpr int kStatusFetchesInFlight = 6;
// The list is recomputed this often (in frames), not every frame: each row costs a cache lookup and a list copy.
constexpr int kStatusRefreshFrames = 10;
// Above this many rows one live fetch per (project, issue type, status) group is enough: it teaches the remembered
// workflow that every sibling row reads.
constexpr size_t kStatusGroupFetchAbove = 8;

bool IsFetching(const TransitionsLookup& lookup) {
    return !lookup.fromLive && (lookup.freshness == smatchet::offline::DataFreshness::Refreshing ||
                                lookup.freshness == smatchet::offline::DataFreshness::LoadingNoCache);
}

// The rows that read the same remembered workflow share this key; empty when the row's workflow cannot be keyed.
std::string StatusGroupKey(const TransitionsQuery& query) {
    return smatchet::workflow::BuildLearnedTransitionsKey(query.ProjectKey, query.IssueTypeKey, query.FromStatusKey);
}

// The pane's tickets by id, built once per pass when the first fetch is about to start.
struct TicketIndex {
    std::shared_ptr<const std::vector<CachedTicket>> Tickets; // keeps the pointers below alive
    std::unordered_map<std::string, const CachedTicket*> ById;
};

TicketIndex IndexPaneTickets(AppController& app, const std::string& paneId) {
    TicketIndex index;
    index.Tickets = app.TicketsSnapshotForTarget(app.LatchPendingActionTargetForPane(paneId));
    if (index.Tickets) {
        index.ById.reserve(index.Tickets->size());
        for (const CachedTicket& ticket : *index.Tickets) {
            index.ById.emplace(ticket.id, &ticket);
        }
    }
    return index;
}

// Starts the live transitions fetches this pass may start: up to kStatusFetchesInFlight in flight, rows taking turns
// from FetchCursor, and above kStatusGroupFetchAbove rows one fetching row per group. Each fetch carries the row's
// status as the grid has it now: the learned workflow is written under the row's status key, so a status the row
// left since the popup opened must not label the answer for the one it has now.
void StartStatusFetches(AppController& app, BulkEditPopupRequest& req, std::vector<TransitionsLookup>& lookups,
                        std::unordered_set<std::string>& covered, bool byGroup, int inFlight) {
    BulkEditPopupRequest::StatusPickCache& cache = req.StatusCache;
    const size_t rowCount = req.StatusQueries.size();
    TicketIndex index;
    bool indexed = false;
    for (size_t k = 0; k < rowCount && inFlight < kStatusFetchesInFlight; ++k) {
        const size_t i = (cache.FetchCursor + k) % rowCount;
        if (lookups[i].fromLive || IsFetching(lookups[i])) {
            continue;
        }
        std::string group = byGroup ? StatusGroupKey(req.StatusQueries[i]) : std::string();
        if (!group.empty()) {
            // One row per group fetches for all of them, and keeps doing so after a failed fetch, so a tracker that
            // errors on one issue is not asked about every sibling in turn.
            const auto rep = cache.GroupRep.find(group);
            if (covered.count(group) != 0 ||
                (rep != cache.GroupRep.end() && rep->second != req.StatusQueries[i].IssueId)) {
                continue;
            }
        }
        if (!indexed) {
            index = IndexPaneTickets(app, req.PaneId);
            indexed = true;
        }
        const auto ticket = index.ById.find(req.StatusQueries[i].IssueId);
        if (ticket != index.ById.end()) {
            req.StatusQueries[i] = smatchet::statuscombo::BuildStatusTransitionsQuery(
                app, *ticket->second, req.Field, ticket->second->GetFieldValue(req.Field.Id));
            const std::string moved = byGroup ? StatusGroupKey(req.StatusQueries[i]) : std::string();
            if (moved != group) { // the row left its group: it no longer speaks for it
                const auto rep = cache.GroupRep.find(group);
                if (rep != cache.GroupRep.end() && rep->second == req.StatusQueries[i].IssueId) {
                    cache.GroupRep.erase(rep);
                }
                group = moved;
            }
        }
        const TransitionsQuery query = req.StatusQueries[i];
        app.EnsureIssueTransitionsLoaded(query);
        lookups[i] = app.GetAvailableTransitionsForIssue(query); // report the fetch this pass, not the next
        cache.FetchCursor = i + 1;
        if (!group.empty()) {
            cache.GroupRep.emplace(group, query.IssueId);
        }
        if (IsFetching(lookups[i]) || lookups[i].fromLive) { // a backed-off or offline row starts nothing
            ++inFlight;
            if (!group.empty()) {
                covered.insert(group);
            }
        }
    }
}

// Recomputes the status picker's list: the statuses every row the job will touch can move to. Each row's list is what
// its own status combo offers (live transitions, else the remembered workflow, else every status; its current status
// stays in it), so a row already at a status never rules that status out. The cue covers the least live row.
//
// The cost of a pass: one cache lookup per row, then one list per DISTINCT non-live answer (rows that read the same
// remembered workflow from the same status share one), so a thousand siblings cost about as much as one.
void RefreshStatusPick(AppController& app, BulkEditPopupRequest& req) {
    BulkEditPopupRequest::StatusPickCache& cache = req.StatusCache;
    cache.Valid = true;
    cache.Applicable = false;
    const size_t rowCount = req.StatusQueries.size();
    if (rowCount == 0) {
        return;
    }
    std::vector<TransitionsLookup> lookups;
    lookups.reserve(rowCount);
    std::unordered_set<std::string> covered; // groups with a live row or a fetch running
    const bool byGroup = rowCount > kStatusGroupFetchAbove;
    int inFlight = 0;
    for (const TransitionsQuery& query : req.StatusQueries) {
        lookups.push_back(app.GetAvailableTransitionsForIssue(query));
        const TransitionsLookup& lookup = lookups.back();
        if (!lookup.applicable) {
            return; // the tracker has no transitions: the catalog's list stands, as for any other choice field
        }
        if (IsFetching(lookup)) {
            ++inFlight;
        }
        if (byGroup && (lookup.fromLive || IsFetching(lookup))) {
            covered.insert(StatusGroupKey(query));
        }
    }
    StartStatusFetches(app, req, lookups, covered, byGroup, inFlight);
    std::vector<std::vector<TrackerFieldOption>> lists;
    std::unordered_set<std::string> seen; // non-live answers already listed (same workflow, status and freshness)
    cache.From = smatchet::statuscombo::StatusOptionsSource::Live;
    cache.Freshness = smatchet::offline::DataFreshness::Fresh;
    for (size_t i = 0; i < rowCount; ++i) {
        const TransitionsQuery& query = req.StatusQueries[i];
        if (!lookups[i].fromLive) {
            const std::string key =
                StatusGroupKey(query) + '\x1f' + query.FromStatusKey + '\x1f' + query.FromStatusName + '\x1f' +
                std::to_string(static_cast<int>(lookups[i].freshness)) + (lookups[i].fromLearned ? 'L' : 'C');
            if (!seen.insert(key).second) {
                continue; // the same list, and so the same intersection and cue, as an earlier row
            }
        }
        smatchet::statuscombo::TicketStatusOptions status =
            smatchet::statuscombo::PickTicketStatusOptions(lookups[i], query, req.Field);
        if (!status.Applicable) {
            return;
        }
        cache.From = smatchet::statuscombo::LessLiveSource(cache.From, status.Pick.From);
        cache.Freshness = smatchet::statuscombo::LeastFresh(cache.Freshness, status.Freshness);
        lists.push_back(std::move(status.Pick.Options));
    }
    cache.Options = smatchet::statuscombo::IntersectStatusOptions(lists);
    cache.Applicable = true;
}

void ResolveStatusOptions(AppController& app, BulkEditPopupRequest& req, OptionList& list) {
    BulkEditPopupRequest::StatusPickCache& cache = req.StatusCache;
    const int frame = ImGui::GetFrameCount();
    if (!cache.Valid || frame < cache.Frame || frame - cache.Frame >= kStatusRefreshFrames) {
        RefreshStatusPick(app, req);
        cache.Frame = frame;
    }
    if (!cache.Applicable) {
        return;
    }
    list.StatusOptions = cache.Options;
    list.IsTransitionList = true;
    list.StatusFrom = cache.From;
    list.Freshness = cache.Freshness;
}

// The components list (the clicked row's own project); any other field keeps the catalog's list.
OptionList ResolveCatalogOptionList(AppController& app, const BulkEditPopupRequest& req) {
    OptionList list;
    list.Options = &req.Field.AllowedValueOptions;
    if (!IsComponentsField(req.Field)) {
        return list;
    }
    // req.Field's options are the catalog's as it was when the popup opened: PickProjectComponents trusts them for this
    // row only when that catalog was fetched for the row's project (the scope captured with the field), never by the
    // live catalog.
    const smatchet::components::ComponentOptionsPick pick = smatchet::components::PickProjectComponents(
        app, smatchet::ExtractIssueKeyPrefix(req.ClickedIssueId), req.Field, req.FieldCatalogProjectKey, list.Holder);
    list.Options = pick.Options;
    list.Freshness = pick.Freshness;
    return list;
}

// What the picker offers this frame: the status rows' common moves (cached, see RefreshStatusPick), else the above.
OptionList ResolveOptionList(AppController& app, BulkEditPopupRequest& req) {
    OptionList list = ResolveCatalogOptionList(app, req);
    if (IsStatusField(req.Field)) {
        ResolveStatusOptions(app, req, list);
    }
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
        const OptionList list = ResolveCatalogOptionList(app, req);
        field.AllowedValueOptions = list.Opts();
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

// The status field's transitions query for each row the job will touch (a skipped row never moves, so it does not
// narrow the list), in row order. Taken once when the popup opens (a pane can hold thousands of tickets): a background
// sync that moves a row meanwhile is harmless, because the job re-reads each row's status when it starts.
void CollectStatusQueries(AppController& app, BulkEditPopupRequest& req) {
    req.StatusQueries.clear();
    const PendingActionTarget target = app.LatchPendingActionTargetForPane(req.PaneId);
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.TicketsSnapshotForTarget(target);
    if (!tickets) {
        return;
    }
    std::unordered_map<std::string, const CachedTicket*> byId;
    for (const BulkRow& row : req.Preview.Rows) {
        if (row.State != BulkRowState::Skipped) {
            byId.emplace(row.IssueId, nullptr);
        }
    }
    for (const CachedTicket& ticket : *tickets) {
        const auto it = byId.find(ticket.id);
        if (it != byId.end()) {
            it->second = &ticket;
        }
    }
    for (const BulkRow& row : req.Preview.Rows) {
        const auto it = byId.find(row.IssueId);
        if (row.State == BulkRowState::Skipped || it == byId.end() || it->second == nullptr) {
            continue;
        }
        req.StatusQueries.push_back(smatchet::statuscombo::BuildStatusTransitionsQuery(
            app, *it->second, req.Field, it->second->GetFieldValue(req.Field.Id)));
    }
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
    req.StatusQueries.clear();
    req.StatusCache = BulkEditPopupRequest::StatusPickCache();
    if (IsStatusField(req.Field)) {
        CollectStatusQueries(e.App, req);
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
    items.reserve(list.Opts().size());
    for (const TrackerFieldOption& option : list.Opts()) {
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

// Apply needs a pick, or Replace with an explicit clear.
bool CanApplyMulti(const BulkEditPopupRequest& req) {
    return !req.Picked.empty() || (req.Mode == BulkMode::Replace && req.Clear);
}

// Apply / Cancel row of the multi-value picker. `enterDone`: Enter finished the tick-box list this frame, which
// is Apply when Apply is enabled. Cancel wins when both fire in one frame: Enter on a focused Cancel button
// reaches the list as a finish too, and must cancel, not submit the pick.
void DrawMultiButtons(PopupEnv& e, bool enterDone) {
    BulkEditPopupRequest& req = e.Req;
    const bool canApply = CanApplyMulti(req);
    ImGui::BeginDisabled(!canApply);
    const bool applyClicked = ImGui::Button(SmatchetLocalization::Label("grid.bulk.apply", "Apply", "bulk_pick_apply"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(SmatchetLocalization::Label("grid.bulk.cancel", "Cancel", "bulk_pick_cancel"))) {
        req.Active = false;
        ImGui::CloseCurrentPopup();
        return;
    }
    if (applyClicked || (enterDone && canApply)) {
        Proceed(e, req.Picked);
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
    const BulkMode modeBefore = req.Mode; // an Enter that switched the mode is not an Apply
    if (multi) {
        DrawModeToggle(req);
    }
    ImGui::Separator();

    const OptionList list = ResolveOptionList(e.App, req);
    if (list.IsTransitionList) {
        smatchet::statuscombo::DrawStatusOptionsCue(list.StatusFrom, list.Freshness); // the status combo's own cue
    } else if (list.Freshness != smatchet::offline::DataFreshness::Fresh) {
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
    } else if (list.IsTransitionList) {
        options.EmptyText =
            SmatchetLocalization::T("grid.bulk.no_common_status", "(no status that every selected row can move to)");
    }
    const smatchet::dropdown::Event ev = smatchet::dropdown::DrawBody(items, options);

    if (multi) {
        ApplyMultiEvent(req, ev);
        ImGui::Separator();
        DrawMultiButtons(e, ev.Kind == smatchet::dropdown::EventKind::Done && req.Mode == modeBefore);
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
