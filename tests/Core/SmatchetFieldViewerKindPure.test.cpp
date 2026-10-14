#include "Ui/SmatchetFieldViewerKindPure.h"

#include <doctest/doctest.h>

using SmatchetFieldViewer::FieldViewerKind;
using SmatchetFieldViewer::FieldViewerKindForFieldId;
using SmatchetFieldViewer::HasFieldViewer;

TEST_CASE("FieldViewerKindForFieldId: comments and history have viewers") {
    CHECK(FieldViewerKindForFieldId("comments") == FieldViewerKind::Comments);
    CHECK(FieldViewerKindForFieldId("history") == FieldViewerKind::ActivityLog);
}

TEST_CASE("FieldViewerKindForFieldId: other fields have none") {
    CHECK(FieldViewerKindForFieldId("description") == FieldViewerKind::None);
    CHECK(FieldViewerKindForFieldId("") == FieldViewerKind::None);
    CHECK(FieldViewerKindForFieldId("comment") == FieldViewerKind::None); // the tooltip blob, not the cell
    CHECK(FieldViewerKindForFieldId("History") == FieldViewerKind::None);
    CHECK(FieldViewerKindForFieldId("Comments") == FieldViewerKind::None);
}

TEST_CASE("HasFieldViewer agrees with FieldViewerKindForFieldId") {
    CHECK(HasFieldViewer("comments"));
    CHECK(HasFieldViewer("history"));
    CHECK_FALSE(HasFieldViewer("description"));
    CHECK_FALSE(HasFieldViewer(""));
    CHECK_FALSE(HasFieldViewer("comment"));
}
