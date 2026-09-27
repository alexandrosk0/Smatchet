// LazyEnrichmentCarryForwardPure doctest — a sync row that lacks the comment thread / tooltip blob
// keeps the ones already cached while the comment count is unchanged (Quality Pillar 6). Pure.

#include "Sync/LazyEnrichmentCarryForwardPure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::sync::CarryForwardLazyCommentFields;
using smatchet::sync::LazyCommentFields;
using smatchet::sync::SnapshotLazyCommentFields;

namespace {

CachedTicket MakeTicket(const std::string& count, const std::string& blob, const std::string& thread) {
    CachedTicket t;
    t.id = "OFF-1";
    t.fieldValues["comments"] = count;
    if (!blob.empty()) {
        t.fieldValues["comment"] = blob;
    }
    if (!thread.empty()) {
        t.fieldRichValues[kCommentThreadRichKey] = thread;
    }
    return t;
}

} // namespace

TEST_CASE("CarryForwardLazyCommentFields — same count copies the missing thread and blob") {
    LazyCommentFields prev;
    REQUIRE(SnapshotLazyCommentFields(MakeTicket("2", "**Ana** 2024-01-15\n\nhi\n", "[{\"id\":\"c1\"}]"), prev));
    CachedTicket incoming = MakeTicket("2", "", "");
    CarryForwardLazyCommentFields(prev, incoming);
    CHECK(incoming.GetFieldRichValue(kCommentThreadRichKey) == "[{\"id\":\"c1\"}]");
    CHECK(incoming.GetFieldValue("comment") == "**Ana** 2024-01-15\n\nhi\n");
}

TEST_CASE("CarryForwardLazyCommentFields — a different count copies nothing") {
    LazyCommentFields prev;
    REQUIRE(SnapshotLazyCommentFields(MakeTicket("2", "old blob", "[{\"id\":\"c1\"}]"), prev));
    CachedTicket incoming = MakeTicket("3", "", "");
    CarryForwardLazyCommentFields(prev, incoming);
    CHECK(incoming.fieldRichValues.find(kCommentThreadRichKey) == incoming.fieldRichValues.end());
    CHECK(incoming.fieldValues.find("comment") == incoming.fieldValues.end());
}

TEST_CASE("CarryForwardLazyCommentFields — values the incoming row has are never replaced") {
    LazyCommentFields prev;
    REQUIRE(SnapshotLazyCommentFields(MakeTicket("1", "old blob", "[\"old\"]"), prev));
    CachedTicket incoming = MakeTicket("1", "new blob", "[\"new\"]");
    CarryForwardLazyCommentFields(prev, incoming);
    CHECK(incoming.GetFieldRichValue(kCommentThreadRichKey) == "[\"new\"]");
    CHECK(incoming.GetFieldValue("comment") == "new blob");

    // Only the thread is missing: it is filled, the fresh blob stays.
    CachedTicket blobOnly = MakeTicket("1", "fresh blob", "");
    CarryForwardLazyCommentFields(prev, blobOnly);
    CHECK(blobOnly.GetFieldRichValue(kCommentThreadRichKey) == "[\"old\"]");
    CHECK(blobOnly.GetFieldValue("comment") == "fresh blob");
}

TEST_CASE("SnapshotLazyCommentFields — nothing to carry without a blob or a thread") {
    LazyCommentFields prev;
    prev.Count = "untouched";
    CHECK_FALSE(SnapshotLazyCommentFields(MakeTicket("0", "", ""), prev));
    CHECK(prev.Count == "untouched");
    CHECK(SnapshotLazyCommentFields(MakeTicket("1", "", "[]"), prev));
    CHECK(prev.Count == "1");
    CHECK(prev.Blob.empty());
}
