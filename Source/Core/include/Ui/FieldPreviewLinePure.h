// FieldPreviewLinePure.h — pure "which line does a one-line grid cell show?" helper.
//
// Grid cells render a single line of a possibly multi-line field value. Taking
// literally the FIRST line makes a cell look EMPTY whenever the value opens with a
// blank or whitespace-only line — the common shape for GitHub issue bodies (a
// leading newline before the first heading), Jira ADF→markdown conversions, and
// pasted descriptions. The ticket has full text; the cell showed nothing.
//
// So the preview is the first line with visible content, not the first line.
//
// Extracted here so it is unit-testable without an ImGui frame
// (tests/Core/FieldPreviewLinePure.test.cpp). Header-only and free of ImGui
// includes on purpose: the bucket-A test TU links none of the UI stack.

#pragma once

#include <cstddef>
#include <string>

namespace smatchet {
namespace field_preview {

struct PreviewLine {
    /// The first line carrying visible content. Empty when the value is empty or
    /// every line is blank — the cell then renders nothing, as before.
    std::string Text;
    /// True when the value spans more than one line, i.e. the cell cannot show all
    /// of it. Drives the overflow tooltip, which always shows the untouched value.
    bool HasMoreLines;

    PreviewLine() : HasMoreLines(false) {}
};

/// Pick the line a one-line cell should preview. Leading blank / whitespace-only
/// lines are skipped; the chosen line is returned verbatim (no trim), so any
/// intentional indent still shows. CRLF counts as one break, not a blank line.
inline PreviewLine FirstVisibleLine(const std::string& value) {
    PreviewLine out;
    out.HasMoreLines = value.find_first_of("\r\n") != std::string::npos;

    std::size_t start = 0;
    while (true) {
        std::size_t end = value.find_first_of("\r\n", start);
        const bool lastLine = (end == std::string::npos);
        if (lastLine) {
            end = value.size();
        }
        const std::string line = value.substr(start, end - start);
        if (line.find_first_not_of(" \t") != std::string::npos) {
            out.Text = line;
            return out;
        }
        if (lastLine) {
            return out;
        }
        start = end + 1;
        if (value[end] == '\r' && start < value.size() && value[start] == '\n') {
            ++start;
        }
    }
}

} // namespace field_preview
} // namespace smatchet
