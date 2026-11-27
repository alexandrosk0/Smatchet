// Pane catalog race -> silent read-only grid: the grid's side. TicketGridColumnsBuilder gives a field its catalog does
// not know the PlainText plan, which RequiresAllowEditsCheck rejects, so a pane whose catalog is empty, not loaded or
// another tracker's renders every cell read-only. Pinned here: the grid's test for that state
// (GridCellsReadOnlyForCatalog / MeasureGridCatalogCoverage) and the cue it shows above the table
// (ChooseGridCatalogCue, TicketGridModel.h). The catalog apply decisions are in BackendSwitchRace1081.test.cpp; the
// controller wiring is in AppControllerFieldCatalog.posix.test.cpp against the real headless AppController.

#include "TicketGridModel.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

TicketGridColumn FieldColumn(const std::string& fieldId, bool resolved) {
    TicketGridColumn column;
    column.ColumnKind = TicketGridColumn::Kind::FieldValue;
    column.Key = "field:" + fieldId;
    column.FieldId = fieldId;
    column.Label = fieldId;
    column.CatalogResolved = resolved;
    return column;
}

TicketGridColumn IdColumn() {
    TicketGridColumn column;
    column.ColumnKind = TicketGridColumn::Kind::Id;
    column.Key = "id";
    column.Label = "ID";
    return column;
}

} // namespace

TEST_CASE("The grid's catalog cue says why the cells are read-only") {
    // Editable: no cue, whatever the fetch is doing.
    CHECK(ChooseGridCatalogCue(false, false, true, true, true) == GridCatalogCue::None);
    // A running fetch wins over a parked one and an older failure.
    CHECK(ChooseGridCatalogCue(true, false, true, true, true) == GridCatalogCue::Loading);
    // A fetch parked until the pane's backend is installed is a wait (it starts on its own), not a failure.
    CHECK(ChooseGridCatalogCue(true, false, false, true, true) == GridCatalogCue::AwaitingBackend);
    CHECK(ChooseGridCatalogCue(true, true, false, true, false) == GridCatalogCue::AwaitingBackend);
    CHECK(ChooseGridCatalogCue(true, false, false, false, true) == GridCatalogCue::Unavailable);
    CHECK(ChooseGridCatalogCue(true, false, false, false, false) == GridCatalogCue::NotLoaded);
    // The pane has its own catalog, and it knows none of the view's fields: loading again would not help.
    CHECK(ChooseGridCatalogCue(true, true, false, false, false) == GridCatalogCue::Unresolved);
}

TEST_CASE("Grid cells are read-only for a catalog reason only when no field column resolved") {
    SUBCASE("no columns, or only the id column: nothing to edit, not a catalog state") {
        CHECK_FALSE(GridCellsReadOnlyForCatalog(MeasureGridCatalogCoverage({})));
        const GridCatalogCoverage idOnly = MeasureGridCatalogCoverage({IdColumn()});
        CHECK(idOnly.FieldColumns == 0);
        CHECK_FALSE(GridCellsReadOnlyForCatalog(idOnly));
    }
    SUBCASE("empty or foreign catalog: every field column unresolved -> read-only") {
        const std::vector<TicketGridColumn> columns = {IdColumn(), FieldColumn("summary", false),
                                                       FieldColumn("status", false),
                                                       FieldColumn("customfield_10010", false)};
        const GridCatalogCoverage coverage = MeasureGridCatalogCoverage(columns);
        CHECK(coverage.FieldColumns == 3);
        CHECK(coverage.ResolvedFieldColumns == 0);
        CHECK(GridCellsReadOnlyForCatalog(coverage));
    }
    SUBCASE("one resolved field column: the catalog is there, so read-only cells are the tracker's verdict") {
        const std::vector<TicketGridColumn> columns = {IdColumn(), FieldColumn("summary", true),
                                                       FieldColumn("customfield_10010", false)};
        const GridCatalogCoverage coverage = MeasureGridCatalogCoverage(columns);
        CHECK(coverage.FieldColumns == 2);
        CHECK(coverage.ResolvedFieldColumns == 1);
        CHECK_FALSE(GridCellsReadOnlyForCatalog(coverage));
    }
}
