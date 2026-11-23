#ifndef SMATCHET_UI_SMATCHET_ACTIVE_PROJECT_GRID_IDS_H
#define SMATCHET_UI_SMATCHET_ACTIVE_PROJECT_GRID_IDS_H

// Identity lookups the grid's selection, range and active cell are kept by (input UX Wave 3, grid S1). A cell is
// drawn by (shown row, drawn column) and kept by (issue id, field id): these two functors answer "who is at this
// index" for SpreadsheetState::Remap / SetRange / ResolveActive. They hold references only and allocate nothing,
// so the per-frame remap and the per-gesture range snapshot cost no copies. Private to the grid partition.

#include "CachedTicketTypes.h"
#include "TicketGridModel.h"

#include <cstddef>
#include <string>
#include <vector>

// The issue id shown at shown row `row`, "" when the row does not exist.
struct GridShownRowIds {
    const std::vector<size_t>& Indices;
    const std::vector<CachedTicket>& Tickets;
    const std::string& operator()(int row) const {
        static const std::string kNoId;
        if (row < 0 || static_cast<size_t>(row) >= Indices.size()) {
            return kNoId;
        }
        const size_t ticketIdx = Indices[static_cast<size_t>(row)];
        return ticketIdx < Tickets.size() ? Tickets[ticketIdx].id : kNoId;
    }
};

// The identity of drawn column `col`: its field id, or "id" for the key column (which has none).
struct GridDrawnColumnFieldIds {
    const std::vector<TicketGridColumn>& Columns;
    const std::string& operator()(int col) const {
        static const std::string kIdField = "id";
        if (col < 0 || static_cast<size_t>(col) >= Columns.size()) {
            return kIdField;
        }
        const TicketGridColumn& column = Columns[static_cast<size_t>(col)];
        return column.ColumnKind == TicketGridColumn::Kind::Id || column.FieldId.empty() ? kIdField : column.FieldId;
    }
};

#endif // SMATCHET_UI_SMATCHET_ACTIVE_PROJECT_GRID_IDS_H
