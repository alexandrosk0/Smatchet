#pragma once

#include "ConfigManager.h"
#include "TicketGridSortPure.h" // CompareFieldValuesForSort / IsTrackerDateOrDateTimeField / IsAttachmentFieldId
#include "TrackerFieldSchema.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

std::string DisplayValueForTrackerDateField(const std::string& fieldId, const TrackerField* field,
                                            const std::string& currentValue, const std::string& dateFormatOption = {},
                                            int thresholdDays = 0);

class TrackerFieldCatalogIndex {
  public:
    explicit TrackerFieldCatalogIndex(const std::vector<TrackerField>& fields);

    const TrackerField* Find(const std::string& fieldId) const;
    std::string DisplayName(const std::string& fieldId) const;

  private:
    std::unordered_map<std::string, const TrackerField*> FieldById;
};

struct TicketGridColumn {
    enum class Kind { Id, FieldValue };

    enum class RenderPlan {
        PlainText,
        SpecialAttachment,
        SpecialWatchers,
        SpecialVotes,
        SpecialWorklog,
        SpecialProgress,
        SpecialIssueRestriction,
        SpecialTimeSpent,
        Labels,
        Cascading,
        MultiSelect,
        SingleSelect,
        DateTimeEditor,
        TextEditor
    };

    Kind ColumnKind = Kind::Id;
    std::string Key;
    std::string Label;
    std::string FieldId;
    RenderPlan Plan = RenderPlan::PlainText;
    bool IsDateLike = false;
    bool CatalogReadOnly = false;
    bool NeedsAllowEditsCheck = false;
    /// The catalog the column was built from knows this field. An unresolved field column renders as plain text,
    /// with no edit affordance, whatever the tracker allows.
    bool CatalogResolved = false;
};

class TicketGridColumnsBuilder {
  public:
    static std::vector<TicketGridColumn> Build(const ViewDefinition& view, const TrackerFieldCatalogIndex& catalog);
};

/// How many field columns a column set has, and how many of them the catalog it was built from resolved.
struct GridCatalogCoverage {
    std::size_t FieldColumns = 0;
    std::size_t ResolvedFieldColumns = 0;
};

/// Counts the field (non-id) columns and the catalog-resolved ones. O(columns), no allocation.
inline GridCatalogCoverage MeasureGridCatalogCoverage(const std::vector<TicketGridColumn>& columns) {
    GridCatalogCoverage coverage;
    for (const TicketGridColumn& column : columns) {
        if (column.ColumnKind != TicketGridColumn::Kind::FieldValue) {
            continue;
        }
        ++coverage.FieldColumns;
        if (column.CatalogResolved) {
            ++coverage.ResolvedFieldColumns;
        }
    }
    return coverage;
}

/// True when every cell of the grid is read-only because of the field catalog, not the tracker: the grid shows
/// field columns and the catalog it was built from resolved none of them (empty, not loaded, or another tracker's).
inline bool GridCellsReadOnlyForCatalog(const GridCatalogCoverage& coverage) {
    return coverage.FieldColumns != 0 && coverage.ResolvedFieldColumns == 0;
}

/// What the grid says above the table while its cells are read-only for a catalog reason.
enum class GridCatalogCue {
    None,            ///< editable (or nothing to edit): no cue
    Loading,         ///< the pane's catalog fetch is running
    AwaitingBackend, ///< the fetch is parked until the pane's tracker backend is installed; it starts on its own
    NotLoaded,       ///< no catalog yet and no fetch running (not started, or waiting on the pane's sync)
    Unavailable, ///< the pane's last catalog fetch failed (PaneCatalogFetchStatus::LastError) or its backend changed
    Unresolved,  ///< the pane has its own catalog, but it knows none of the view's fields
};

/// Picks the cue. `ownCatalogPopulated`: the pane's own catalog is loaded or restored. `fetchInFlight` /
/// `fetchAwaitingBackend` / `fetchFailed`: the state of the pane's own catalog fetch
/// (AppController::PaneCatalogFetchStatus). A running fetch wins over a parked one, and both are waits that win over an
/// older failure.
inline GridCatalogCue ChooseGridCatalogCue(bool cellsReadOnlyForCatalog, bool ownCatalogPopulated, bool fetchInFlight,
                                           bool fetchAwaitingBackend, bool fetchFailed) {
    if (!cellsReadOnlyForCatalog) {
        return GridCatalogCue::None;
    }
    if (fetchInFlight) {
        return GridCatalogCue::Loading;
    }
    if (fetchAwaitingBackend) {
        return GridCatalogCue::AwaitingBackend;
    }
    if (fetchFailed) {
        return GridCatalogCue::Unavailable;
    }
    return ownCatalogPopulated ? GridCatalogCue::Unresolved : GridCatalogCue::NotLoaded;
}
