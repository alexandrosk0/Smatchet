// GridCopyPure.h — pure row-mapping + CSV-line helpers behind the Key cell's "Copy N selected
// rows as CSV" menu item (SmatchetGridUiSupport.cpp, BuildGridRowsCsv).
//
// Header-only and standard-library-only on purpose: the cell-value lookup lives in
// TicketGridModel.cpp, which is ImGui-coupled and cannot link into the unit tests, so only the
// row mapping and the CSV join are lifted here (tests/Core/GridCopyPure.test.cpp).
#pragma once

#include "StringUtil.h"

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

/// Comma-join of CsvEscapeField(cell) for each cell, terminated with '\n' (RFC 4180 quoting but
/// LF, not CRLF, line endings — spreadsheets and text editors accept both).
inline std::string BuildCsvLine(const std::vector<std::string>& cells) {
    std::string out;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += CsvEscapeField(cells[i]);
    }
    out.push_back('\n');
    return out;
}
