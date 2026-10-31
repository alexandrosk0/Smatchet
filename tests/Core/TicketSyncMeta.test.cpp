// TicketSyncMeta — bucket-A coverage for the pure rules the delta sync stores beside each cached row
// (Source/Core/include/TicketSyncMeta.h, docs/plans/active/delta-sync.md § Phase 2): building the meta
// from a fetched row, the "does this row carry what my view needs" subset check, and the same-version
// merge that keeps two panes' columns from erasing each other.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "TicketSyncMeta.h"

using smatchet::sync::BuildSyncMeta;
using smatchet::sync::FieldsOfRow;
using smatchet::sync::FieldsSatisfy;
using smatchet::sync::IsSameVersion;
using smatchet::sync::MergeSameVersionRow;
using smatchet::sync::NormalizeFieldList;
using smatchet::sync::UnionFields;

namespace {
CachedTicket Row(const std::string& id, const std::string& updated) {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = "s-" + id;
    t.fieldValues["updated"] = updated;
    return t;
}
} // namespace

TEST_CASE("BuildSyncMeta stamps the row's updated value, its sorted field ids and the save time") {
    CachedTicket t = Row("A-1", "2024-05-01T10:00:00.000+0000");
    t.fieldValues["status"] = "Open";
    const TicketSyncMeta meta = BuildSyncMeta(t, 1234);
    CHECK(meta.RemoteUpdated == "2024-05-01T10:00:00.000+0000");
    CHECK(meta.Fields == std::vector<std::string>{"status", "summary", "updated"});
    CHECK(meta.SyncedAtEpochSec == 1234);
}

TEST_CASE("BuildSyncMeta of a row without updated has an empty stamp (unknown version)") {
    CachedTicket t;
    t.id = "A-2";
    t.fieldValues["summary"] = "x";
    CHECK(BuildSyncMeta(t, 1).RemoteUpdated.empty());
}

TEST_CASE("NormalizeFieldList and UnionFields give sorted unique lists") {
    CHECK(NormalizeFieldList({"b", "a", "b"}) == std::vector<std::string>{"a", "b"});
    CHECK(UnionFields({"a", "c"}, {"b", "c"}) == std::vector<std::string>{"a", "b", "c"});
    CHECK(UnionFields({}, {}).empty());
}

TEST_CASE("FieldsSatisfy is a subset check: a row serves any view whose fields it carries") {
    const std::vector<std::string> row = {"assignee", "status", "summary", "updated"};
    CHECK(FieldsSatisfy(row, {"summary", "status"}));
    CHECK(FieldsSatisfy(row, {}));
    CHECK(FieldsSatisfy(row, {"", "summary"})); // blank ids are ignored
    CHECK_FALSE(FieldsSatisfy(row, {"summary", "labels"}));
    CHECK_FALSE(FieldsSatisfy({}, {"summary"}));
}

TEST_CASE("Two panes with different columns satisfy each other once their fields are merged") {
    // The reason the list is stored, not a hash: pane A needs {status}, pane B needs {labels}.
    const std::vector<std::string> a = {"status", "summary"};
    const std::vector<std::string> b = {"labels", "summary"};
    CHECK_FALSE(FieldsSatisfy(a, b));
    CHECK_FALSE(FieldsSatisfy(b, a));
    const std::vector<std::string> merged = UnionFields(a, b);
    CHECK(FieldsSatisfy(merged, a));
    CHECK(FieldsSatisfy(merged, b));
}

TEST_CASE("MergeSameVersionRow keeps the cached row's other fields and lets the incoming values win") {
    CachedTicket existing = Row("A-1", "T1");
    existing.fieldValues["status"] = "Open";
    existing.fieldValues["summary"] = "old summary";
    existing.fieldRichValues["description"] = "{adf}";
    CachedTicket incoming = Row("A-1", "T1");
    incoming.fieldValues["labels"] = "x";
    incoming.fieldValues["summary"] = "new summary";

    const CachedTicket merged = MergeSameVersionRow(existing, incoming);
    CHECK(merged.id == "A-1");
    CHECK(merged.fieldValues.at("summary") == "new summary"); // incoming wins
    CHECK(merged.fieldValues.at("status") == "Open");         // kept from the cached row
    CHECK(merged.fieldValues.at("labels") == "x");
    CHECK(merged.fieldRichValues.at("description") == "{adf}");
    CHECK(FieldsOfRow(merged) == std::vector<std::string>{"labels", "status", "summary", "updated"});
}

TEST_CASE("IsSameVersion needs two known, equal stamps") {
    TicketSyncMeta a;
    a.RemoteUpdated = "T1";
    TicketSyncMeta b;
    b.RemoteUpdated = "T1";
    CHECK(IsSameVersion(a, b));
    b.RemoteUpdated = "T2";
    CHECK_FALSE(IsSameVersion(a, b));
    a.RemoteUpdated.clear();
    b.RemoteUpdated.clear();
    CHECK_FALSE(IsSameVersion(a, b)); // unknown never counts as "same"
}

TEST_CASE("IsSameVersion treats two renderings of one instant as the same version") {
    TicketSyncMeta a;
    a.RemoteUpdated = "2024-05-01T10:00:00.000+0000";
    TicketSyncMeta b;
    b.RemoteUpdated = "2024-05-01T12:00:00.000+0200";
    CHECK(IsSameVersion(a, b));
    b.RemoteUpdated = "2024-05-01T12:00:01.000+0200";
    CHECK_FALSE(IsSameVersion(a, b));
}
