#include "TicketGridModel.h"

#include "CompactDateFormat.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerLabelsEditor.h"
#include "StringUtil.h"

#include <algorithm>
#include <iterator>
#include <unordered_set>

namespace {

// Returns a Special* render plan for id-only columns (field == null) by column-id
// predicate, or PlainText when nothing special matches.
TicketGridColumn::RenderPlan ResolveRenderPlanForFieldId(const std::string& fieldId) {
    if (IsAttachmentFieldId(fieldId)) {
        return TicketGridColumn::RenderPlan::SpecialAttachment;
    }
    if (TrackerGridFieldDisplay::IsWatchersColumnId(fieldId)) {
        return TicketGridColumn::RenderPlan::SpecialWatchers;
    }
    if (TrackerGridFieldDisplay::IsVotesColumnId(fieldId)) {
        return TicketGridColumn::RenderPlan::SpecialVotes;
    }
    if (TrackerGridFieldDisplay::IsWorklogColumnId(fieldId)) {
        return TicketGridColumn::RenderPlan::SpecialWorklog;
    }
    if (TrackerGridFieldDisplay::IsProgressStyleColumnId(fieldId)) {
        return TicketGridColumn::RenderPlan::SpecialProgress;
    }
    if (TrackerGridFieldDisplay::IsIssueRestrictionColumnId(fieldId)) {
        return TicketGridColumn::RenderPlan::SpecialIssueRestriction;
    }
    return TicketGridColumn::RenderPlan::PlainText;
}

// Special* render plans that apply to a resolved field by id/metadata predicate, before
// edit-affordance dispatch. Returns true (with `out` set) when a special plan matches.
bool TryResolveSpecialFieldPlan(const TrackerField* field, TicketGridColumn::RenderPlan& out) {
    if (IsAttachmentFieldId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialAttachment;
        return true;
    }
    if (TrackerGridFieldDisplay::IsWatchersColumnId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialWatchers;
        return true;
    }
    if (TrackerGridFieldDisplay::IsVotesColumnId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialVotes;
        return true;
    }
    if (TrackerGridFieldDisplay::IsWorklogColumnId(field->Id)) {
        out = TicketGridColumn::RenderPlan::SpecialWorklog;
        return true;
    }
    if (TrackerGridFieldDisplay::IsProgressDisplayField(field)) {
        out = TicketGridColumn::RenderPlan::SpecialProgress;
        return true;
    }
    if (TrackerGridFieldDisplay::IsIssueRestrictionField(field)) {
        out = TicketGridColumn::RenderPlan::SpecialIssueRestriction;
        return true;
    }
    return false;
}

// Edit-affordance dispatch for a resolved, editable (non-special, non-read-only) field.
TicketGridColumn::RenderPlan ResolveEditableFieldPlan(const TrackerField* field) {
    if (TrackerLabelsEditor::IsLabelsField(field->Id)) {
        return TicketGridColumn::RenderPlan::Labels;
    }
    if (field->Family == TrackerFieldFamily::CascadingSelect) {
        return TicketGridColumn::RenderPlan::Cascading;
    }
    if ((field->Family == TrackerFieldFamily::SelectMulti || field->Family == TrackerFieldFamily::StructuredMulti ||
         field->Family == TrackerFieldFamily::UserMulti) &&
        !field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::MultiSelect;
    }
    if ((field->Family == TrackerFieldFamily::SelectSingle || field->Family == TrackerFieldFamily::StructuredSingle ||
         field->Family == TrackerFieldFamily::UserSingle || field->Family == TrackerFieldFamily::Status ||
         field->Family == TrackerFieldFamily::IssueType) &&
        !field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::SingleSelect;
    }
    if (field->IsArray && !field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::MultiSelect;
    }
    // Fallback B: components on an unresolvable-JQL view (filter-id / cross-project / non-`project=`)
    // can't be enriched, so AllowedValueOptions stays empty. Still render an (empty) MultiSelect
    // dropdown rather than a text editor — it lazily populates once a project-scoped catalog lands.
    // The common `project = X` case populates real options via the scoped catalog save/load path
    // and matches the non-empty MultiSelect branch above; this is the degraded-only path.
    if (field->IsArray &&
        (field->Family == TrackerFieldFamily::SelectMulti || ToLowerAsciiCopy(field->ItemsType) == "component")) {
        return TicketGridColumn::RenderPlan::MultiSelect;
    }
    if (!field->AllowedValueOptions.empty()) {
        return TicketGridColumn::RenderPlan::SingleSelect;
    }
    if (TrackerDateTimeFieldEditor::IsTrackerDateTimePickerField(*field)) {
        return TicketGridColumn::RenderPlan::DateTimeEditor;
    }
    return TicketGridColumn::RenderPlan::TextEditor;
}

TicketGridColumn::RenderPlan ResolveRenderPlan(const std::string& fieldId, const TrackerField* field) {
    if (fieldId == "timespent" || (field && field->Id == "timespent")) {
        return TicketGridColumn::RenderPlan::SpecialTimeSpent;
    }
    if (field == nullptr) {
        return ResolveRenderPlanForFieldId(fieldId);
    }

    TicketGridColumn::RenderPlan special = TicketGridColumn::RenderPlan::PlainText;
    if (TryResolveSpecialFieldPlan(field, special)) {
        return special;
    }
    if (field->ReadOnly) {
        return TicketGridColumn::RenderPlan::PlainText;
    }
    return ResolveEditableFieldPlan(field);
}

bool RequiresAllowEditsCheck(TicketGridColumn::RenderPlan plan) {
    return plan == TicketGridColumn::RenderPlan::Labels || plan == TicketGridColumn::RenderPlan::Cascading ||
           plan == TicketGridColumn::RenderPlan::MultiSelect || plan == TicketGridColumn::RenderPlan::SingleSelect ||
           plan == TicketGridColumn::RenderPlan::DateTimeEditor || plan == TicketGridColumn::RenderPlan::TextEditor;
}

} // namespace

std::string DisplayValueForTrackerDateField(const std::string& fieldId, const TrackerField* field,
                                            const std::string& currentValue, const std::string& dateFormatOption,
                                            int thresholdDays) {
    bool isDate = IsTrackerDateOrDateTimeField(fieldId, field);
    if (!isDate) {
        ParsedJiraDateTime dummy;
        if (TryParseJiraDateTime(currentValue, dummy)) {
            isDate = true;
        }
    }
    if (!isDate) {
        return currentValue;
    }
    // Use caller-supplied format params if provided; only hit disk when called from non-hot paths.
    std::string fmt = dateFormatOption;
    int thresh = thresholdDays;
    if (fmt.empty() || thresh <= 0) {
        const auto cfg = ConfigManager::Load();
        if (fmt.empty())
            fmt = cfg.DateFormatOption;
        if (thresh <= 0)
            thresh = cfg.DateCompactRelativeThresholdDays;
    }
    const std::string compact = FormatCompactJiraDateForDisplay(currentValue, fmt, thresh);
    return compact.empty() ? currentValue : compact;
}

TrackerFieldCatalogIndex::TrackerFieldCatalogIndex(const std::vector<TrackerField>& fields) {
    for (const auto& field : fields) {
        FieldById[field.Id] = &field;
    }
}

const TrackerField* TrackerFieldCatalogIndex::Find(const std::string& fieldId) const {
    const auto it = FieldById.find(fieldId);
    return it == FieldById.end() ? nullptr : it->second;
}

std::string TrackerFieldCatalogIndex::DisplayName(const std::string& fieldId) const {
    if (fieldId == "history") {
        return "History";
    }
    const TrackerField* field = Find(fieldId);
    return field ? field->Name : fieldId;
}

std::vector<TicketGridColumn> TicketGridColumnsBuilder::Build(const ViewDefinition& view,
                                                              const TrackerFieldCatalogIndex& catalog) {
    std::vector<TicketGridColumn> columns;
    std::vector<TicketGridColumn> allColumns;
    allColumns.push_back({TicketGridColumn::Kind::Id, "id", "ID", std::string()});

    std::unordered_set<std::string> seenFieldIds;
    for (const auto& rawFieldId : view.Fields) {
        // issue-comments fix (#1291) — fold Jira's legacy `comment` column onto the unified `comments`
        // cell so a view saved before the dedupe renders the count/modal (not the raw ADF blob) and
        // dedups against an explicit `comments` column via seenFieldIds below. See CanonicalizeGridFieldId.
        const std::string fieldId = CanonicalizeGridFieldId(TrimCopyAsciiWhitespace(rawFieldId));
        if (fieldId.empty() || !seenFieldIds.insert(fieldId).second) {
            continue;
        }

        TicketGridColumn column;
        column.ColumnKind = TicketGridColumn::Kind::FieldValue;
        column.Key = "field:" + fieldId;
        column.FieldId = fieldId;
        column.Label = catalog.DisplayName(fieldId);
        const TrackerField* field = catalog.Find(fieldId);
        column.Plan = ResolveRenderPlan(fieldId, field);
        column.IsDateLike = IsTrackerDateOrDateTimeField(fieldId, field);
        column.CatalogReadOnly = field != nullptr && field->ReadOnly;
        column.NeedsAllowEditsCheck = RequiresAllowEditsCheck(column.Plan);
        allColumns.push_back(column);
    }

    std::unordered_map<std::string, TicketGridColumn> byKey;
    for (const auto& col : allColumns) {
        byKey[col.Key] = col;
    }

    std::unordered_set<std::string> usedKeys;
    for (const auto& rawKey : view.ColumnOrder) {
        // Canonicalize the saved order key the same way the column Key was built from view.Fields
        // above (ASCII-whitespace trim + legacy-alias fold, e.g. `field:comment` → `field:comments`,
        // #1291). A pre-canonicalization view — or one carrying stray whitespace — then keeps each
        // column's saved position instead of dropping it to the appended tail; the dedup guard drops
        // a ColumnOrder that lists the same canonical key twice (ticketgrid-columnorder-canon).
        const std::string key = CanonicalGridColumnKey(rawKey);
        const auto it = byKey.find(key);
        if (it == byKey.end() || !usedKeys.insert(key).second) {
            continue;
        }
        columns.push_back(it->second);
    }
    std::copy_if(allColumns.begin(), allColumns.end(), std::back_inserter(columns),
                 [&](const TicketGridColumn& col) { return usedKeys.find(col.Key) == usedKeys.end(); });

    return columns;
}
