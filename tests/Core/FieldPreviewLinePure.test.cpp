// FieldPreviewLinePure.test.cpp — bucket-A coverage for the one-line cell preview picker.
//
// Production site: Source/Core/src/Ui/SmatchetFieldRender.cpp — RenderClippedFieldText,
// the single choke point every one-line field cell draws through (description cells,
// generic field cells, the saving-state cell, the mobile detail pane, every
// TrackerGridFieldDisplay row).
//
// The defect this pins: taking literally the first line rendered an EMPTY cell for any
// value opening with a blank line — the routine shape of a GitHub issue body, so tickets
// with full text showed nothing in the Body column.
//
// HasMoreLines must keep the OLD semantics (does the value contain any line break at
// all), because it drives the overflow tooltip; only the drawn line moved.

#include "FieldPreviewLinePure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::field_preview::FirstVisibleLine;
using smatchet::field_preview::PreviewLine;

TEST_CASE("FirstVisibleLine returns single-line values untouched") {
    const PreviewLine empty = FirstVisibleLine("");
    CHECK(empty.Text == "");
    CHECK_FALSE(empty.HasMoreLines);

    const PreviewLine plain = FirstVisibleLine("Crash on save");
    CHECK(plain.Text == "Crash on save");
    CHECK_FALSE(plain.HasMoreLines);
}

TEST_CASE("FirstVisibleLine truncates at the first break and flags the remainder") {
    const PreviewLine lf = FirstVisibleLine("Summary line\nDetail body");
    CHECK(lf.Text == "Summary line");
    CHECK(lf.HasMoreLines);

    const PreviewLine crlf = FirstVisibleLine("Summary line\r\nDetail body");
    CHECK(crlf.Text == "Summary line");
    CHECK(crlf.HasMoreLines);

    // A value that merely ENDS in a newline still reports more lines — the old
    // hasNewline did, and the tooltip trigger must not change.
    const PreviewLine trailing = FirstVisibleLine("Summary line\n");
    CHECK(trailing.Text == "Summary line");
    CHECK(trailing.HasMoreLines);
}

TEST_CASE("FirstVisibleLine skips leading blank lines — the GitHub-body case") {
    // The reported bug: body opens with a newline, cell drew nothing.
    const PreviewLine leadingLf = FirstVisibleLine("\n## Steps to reproduce\nOpen the grid");
    CHECK(leadingLf.Text == "## Steps to reproduce");
    CHECK(leadingLf.HasMoreLines);

    const PreviewLine leadingCrlf = FirstVisibleLine("\r\n## Steps to reproduce");
    CHECK(leadingCrlf.Text == "## Steps to reproduce");
    CHECK(leadingCrlf.HasMoreLines);

    // Several blanks, and a whitespace-only line, are all skipped: they render as
    // nothing, which is exactly the empty cell being fixed.
    const PreviewLine several = FirstVisibleLine("\n\n   \n\t\nHello");
    CHECK(several.Text == "Hello");
    CHECK(several.HasMoreLines);
}

TEST_CASE("FirstVisibleLine returns the chosen line verbatim") {
    // No trim: an intentional indent is content, and trimming would shift the cell
    // text away from what the tooltip and the editor show.
    const PreviewLine indented = FirstVisibleLine("\n    indented code\nrest");
    CHECK(indented.Text == "    indented code");
    CHECK(indented.HasMoreLines);
}

TEST_CASE("FirstVisibleLine yields nothing when every line is blank") {
    // Nothing visible to show — the cell renders empty, as it did before.
    const PreviewLine blanks = FirstVisibleLine("\n\n");
    CHECK(blanks.Text == "");
    CHECK(blanks.HasMoreLines);

    const PreviewLine spaces = FirstVisibleLine("   \t  ");
    CHECK(spaces.Text == "");
    CHECK_FALSE(spaces.HasMoreLines);
}
