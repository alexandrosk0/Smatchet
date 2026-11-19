#include "Ui/BulkFieldEditPure.h"

#include "SmatchetLocalization.h"
#include "StringUtil.h"
#include "Tracker/TrackerFieldValueUtils.h"
#include "Tracker/TrackerFieldOptionLookup.h"
#include "Tracker/TrackerLabelsPure.h"

#include <algorithm>
#include <set>

namespace smatchet {
namespace grid {
namespace bulk {

namespace {

bool SameEntry(const std::string& a, const std::string& b, bool caseInsensitive) {
    return caseInsensitive ? ToLowerAsciiCopy(a) == ToLowerAsciiCopy(b) : a == b;
}

bool ContainsEntry(const std::vector<std::string>& list, const std::string& value, bool caseInsensitive) {
    return std::any_of(list.begin(), list.end(),
                       [&](const std::string& entry) { return SameEntry(entry, value, caseInsensitive); });
}

bool IsFinal(BulkRowState state) { return state != BulkRowState::Pending && state != BulkRowState::Saving; }

} // namespace

BulkFieldKind ClassifyBulkField(const TrackerField& field) {
    if (field.ReadOnly) {
        return BulkFieldKind::Unsupported;
    }
    if (ToLowerAsciiCopy(field.Id) == "labels") {
        return BulkFieldKind::Labels;
    }
    const TrackerFieldFamily family = field.Family;
    if (family == TrackerFieldFamily::CascadingSelect) {
        return BulkFieldKind::Unsupported;
    }
    const bool hasOptions = !field.AllowedValueOptions.empty();
    if ((family == TrackerFieldFamily::SelectMulti || family == TrackerFieldFamily::StructuredMulti ||
         family == TrackerFieldFamily::UserMulti) &&
        hasOptions) {
        return BulkFieldKind::MultiChoice;
    }
    if ((family == TrackerFieldFamily::SelectSingle || family == TrackerFieldFamily::StructuredSingle ||
         family == TrackerFieldFamily::UserSingle || family == TrackerFieldFamily::Status ||
         family == TrackerFieldFamily::IssueType) &&
        hasOptions) {
        return BulkFieldKind::SingleChoice;
    }
    if (field.IsArray && hasOptions) {
        return BulkFieldKind::MultiChoice;
    }
    // Components on a view whose catalog could not be scoped keep an empty list that fills in lazily.
    if (field.IsArray &&
        (family == TrackerFieldFamily::SelectMulti || ToLowerAsciiCopy(field.ItemsType) == "component")) {
        return BulkFieldKind::MultiChoice;
    }
    if (hasOptions) {
        return BulkFieldKind::SingleChoice;
    }
    return BulkFieldKind::Unsupported;
}

bool IsMultiValueKind(BulkFieldKind kind) {
    return kind == BulkFieldKind::MultiChoice || kind == BulkFieldKind::Labels;
}

BulkSummary Summarize(const BulkFieldEditJob& job) {
    BulkSummary summary;
    summary.Total = static_cast<int>(job.Rows.size());
    for (const BulkRow& row : job.Rows) {
        switch (row.State) {
        case BulkRowState::Saved:
            ++summary.Saved;
            break;
        case BulkRowState::Queued:
            ++summary.Queued;
            break;
        case BulkRowState::Failed:
            ++summary.Failed;
            break;
        case BulkRowState::Skipped:
            ++summary.Skipped;
            break;
        case BulkRowState::Pending:
        case BulkRowState::Saving:
            ++summary.Remaining;
            break;
        }
    }
    summary.Done = summary.Remaining == 0;
    return summary;
}

bool SetRowState(BulkFieldEditJob& job, const std::string& issueId, BulkRowState state, const std::string& message) {
    for (BulkRow& row : job.Rows) {
        if (row.IssueId != issueId) {
            continue;
        }
        if (IsFinal(row.State)) {
            return true; // known row, already reported: keep its first outcome
        }
        row.State = state;
        row.Message = message;
        return true;
    }
    return false;
}

std::vector<std::string> ComputeValuesForRow(BulkMode mode, const std::vector<std::string>& before,
                                             const std::vector<std::string>& picked, bool caseInsensitive) {
    std::vector<std::string> out;
    const auto addUnique = [&](const std::string& value) {
        if (!value.empty() && !ContainsEntry(out, value, caseInsensitive)) {
            out.push_back(value);
        }
    };
    switch (mode) {
    case BulkMode::Replace:
        for (const std::string& value : picked) {
            addUnique(value);
        }
        break;
    case BulkMode::Add:
        for (const std::string& value : before) {
            addUnique(value);
        }
        for (const std::string& value : picked) {
            addUnique(value);
        }
        break;
    case BulkMode::Remove:
        for (const std::string& value : before) {
            if (!ContainsEntry(picked, value, caseInsensitive)) {
                addUnique(value);
            }
        }
        break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

int DistinctBeforeValueCount(const BulkFieldEditJob& job) {
    std::set<std::string> distinct;
    for (const BulkRow& row : job.Rows) {
        if (row.State != BulkRowState::Skipped) {
            distinct.insert(row.BeforeValue);
        }
    }
    return static_cast<int>(distinct.size());
}

std::string MixedValueLabel(const BulkFieldEditJob& job) {
    const int distinct = DistinctBeforeValueCount(job);
    if (distinct == 0) {
        return std::string();
    }
    if (distinct == 1) {
        for (const BulkRow& row : job.Rows) {
            if (row.State != BulkRowState::Skipped) {
                return row.BeforeValue;
            }
        }
    }
    return "mixed (" + std::to_string(distinct) + " values)";
}

bool NeedsConfirm(int rowCount, int threshold) { return rowCount > threshold; }

std::vector<std::string> BeforeValuesFor(const TrackerField& field, BulkFieldKind kind, const std::string& value) {
    if (value.empty()) {
        return std::vector<std::string>();
    }
    if (kind == BulkFieldKind::Labels) {
        std::vector<std::string> labels;
        for (const std::string& label : TrackerLabelsPure::ParseCsv(value)) {
            const std::string trimmed = TrimCopy(label);
            if (!trimmed.empty()) {
                labels.push_back(trimmed);
            }
        }
        return labels;
    }
    if (kind == BulkFieldKind::MultiChoice) {
        return TrackerFieldValueUtils::ResolveCurrentSelectionIds(field, value);
    }
    const std::string id = TrackerFieldValueUtils::ResolveOptionId(field, value);
    return id.empty() ? std::vector<std::string>() : std::vector<std::string>{id};
}

std::vector<std::string> NewValuesForRow(BulkFieldKind kind, BulkMode mode, const std::vector<std::string>& before,
                                         const std::vector<std::string>& picked) {
    if (kind == BulkFieldKind::SingleChoice) {
        return picked;
    }
    return ComputeValuesForRow(mode, before, picked, kind == BulkFieldKind::Labels);
}

bool AllEntriesResolve(const TrackerField& field, const std::vector<std::string>& entries) {
    for (const std::string& entry : entries) {
        if (tracker_field_options::FindByIdOrName(field.AllowedValueOptions, entry) == nullptr) {
            return false;
        }
    }
    return true;
}

bool IsRowUnchanged(BulkFieldKind kind, const std::vector<std::string>& before, const std::vector<std::string>& after) {
    if (kind == BulkFieldKind::SingleChoice) {
        return before == after;
    }
    // A multi value compares as a normalised set: the editors write it sorted and deduplicated.
    return ComputeValuesForRow(BulkMode::Replace, before, before, kind == BulkFieldKind::Labels) == after;
}

std::string BulkSummaryText(const BulkSummary& s) {
    std::string out;
    const auto append = [&out](const char* text) {
        if (!out.empty()) {
            out += ", ";
        }
        out += text;
    };
    if (s.Saved > 0) {
        append(SmatchetLocalization::Format("grid.bulk.n_saved", "%d saved", s.Saved));
    }
    if (s.Queued > 0) {
        append(SmatchetLocalization::Format("grid.bulk.n_queued", "%d queued", s.Queued));
    }
    if (s.Failed > 0) {
        append(SmatchetLocalization::Format("grid.bulk.n_failed", "%d failed", s.Failed));
    }
    if (s.Skipped > 0) {
        append(SmatchetLocalization::Format("grid.bulk.n_skipped", "%d skipped", s.Skipped));
    }
    return out;
}

ToastType BulkSummaryToastType(const BulkSummary& s) {
    if (s.Failed > 0) {
        return (s.Saved + s.Queued) > 0 ? ToastType::Warning : ToastType::Error;
    }
    return s.Queued > 0 ? ToastType::Info : ToastType::Success;
}

} // namespace bulk
} // namespace grid
} // namespace smatchet
