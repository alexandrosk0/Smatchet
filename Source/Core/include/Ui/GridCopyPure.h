// GridCopyPure.h — pure row-mapping + CSV-line / TSV-cell helpers behind the Key cell's "Copy N
// selected rows as CSV" menu item and the Ctrl+C TSV copy (SmatchetGridUiSupport.cpp, BuildGridRowsCsv
// and CopyGridRectAsTsv).
//
// Header-only and standard-library-only on purpose: the cell-value lookup lives in
// TicketGridModel.cpp, which is ImGui-coupled and cannot link into the unit tests, so only the
// row mapping and the CSV join are lifted here (tests/Core/GridCopyPure.test.cpp).
#pragma once

#include "StringUtil.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

/// Sort-order row indices (ascending) -> ticket indices; same mapping as CopyGridRectAsTsv
/// (sortedIdx[r] when sortedIdx is non-empty, identity otherwise). Negative rows, rows past
/// sortedIdx and mapped indices >= ticketCount are skipped.
inline std::vector<std::size_t> ResolveGridRowsToTicketIndices(const std::set<int>& rows,
                                                               const std::vector<std::size_t>& sortedIdx,
                                                               std::size_t ticketCount) {
    std::vector<std::size_t> out;
    out.reserve(rows.size());
    const bool useSorted = !sortedIdx.empty();
    for (int r : rows) {
        if (r < 0) {
            continue;
        }
        std::size_t ticketIdx = static_cast<std::size_t>(r);
        if (useSorted) {
            if (ticketIdx >= sortedIdx.size()) {
                continue;
            }
            ticketIdx = sortedIdx[ticketIdx];
        }
        if (ticketIdx >= ticketCount) {
            continue;
        }
        out.push_back(ticketIdx);
    }
    return out;
}

/// Comma-join of each cell for the clipboard: tabs become spaces, the cell is formula-guarded
/// (GuardSpreadsheetFormula: tracker text that starts like a formula is pasted as text) and then RFC
/// 4180-quoted (CsvEscapeField); terminated with '\n' (LF, not CRLF, line endings — spreadsheets and
/// text editors accept both). The tabs go because a spreadsheet splits pasted text on them, quotes or
/// not: "'\t=cmd" would land as a "'" cell next to a live "=cmd" cell.
inline std::string BuildCsvLine(const std::vector<std::string>& cells) {
    std::string out;
    std::string cell;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        cell = cells[i];
        std::replace(cell.begin(), cell.end(), '\t', ' ');
        out += CsvEscapeField(GuardSpreadsheetFormula(cell));
    }
    out.push_back('\n');
    return out;
}
