// AiContextBuilder pure-logic tests. Exercises Source/Core/include/AiContextBuilder.h —
// the 5 block builders + BuildAll filtering by enable flags + MergeEnabled tag wrapping
// + selection-row cap behaviour preserving sort order.
//
// Pure C++14, no AppController link. The `Inputs` carry a `shared_ptr<vector<CachedTicket>>`
// snapshot directly so tests construct their own fixtures.

#include "AiContextBuilder.h"
#include "AiTypes.h"
#include "CachedTicketTypes.h"
#include "ConfigManager.h" // for ViewDefinition

#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

std::shared_ptr<std::vector<CachedTicket>> MakeTickets(int n) {
    auto out = std::make_shared<std::vector<CachedTicket>>();
    out->reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        CachedTicket t;
        t.id = "SMA-" + std::to_string(100 + i);
        t.fieldValues["summary"] = "Ticket #" + std::to_string(i);
        t.fieldValues["status"] = (i % 2) ? "In Progress" : "Open";
        out->push_back(std::move(t));
    }
    return out;
}

std::vector<std::size_t> IdentitySort(std::size_t n) {
    std::vector<std::size_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(i);
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("AiContextBuilder::BuildAll — empty state emits 5 blocks with empty bodies") {
    AiContextBuilder::Inputs in;
    // Disable audit trail explicitly — the default ReadRecentEvents path can pick up
    // an audit file from the test runner's environment. The other 4 blocks naturally
    // produce empty bodies when their input pointers are null.
    in.EnableAuditTrail = false;
    const auto blocks = AiContextBuilder::BuildAll(in);
    REQUIRE(blocks.size() == 5);
    CHECK(static_cast<int>(blocks[0].Kind) == static_cast<int>(AiContextBlockKind::MultiSelectedTickets));
    CHECK(static_cast<int>(blocks[1].Kind) == static_cast<int>(AiContextBlockKind::VisibleGridRows));
    CHECK(static_cast<int>(blocks[2].Kind) == static_cast<int>(AiContextBlockKind::ActiveTicket));
    CHECK(static_cast<int>(blocks[3].Kind) == static_cast<int>(AiContextBlockKind::ActiveView));
    CHECK(static_cast<int>(blocks[4].Kind) == static_cast<int>(AiContextBlockKind::AuditTrail));
    for (const auto& b : blocks) {
        CHECK(b.Body.empty());
        CHECK_FALSE(b.Name.empty());
    }
    CHECK(AiContextBuilder::MergeEnabled(in).empty());
}

TEST_CASE("AiContextBuilder::BuildSelectionBody — caps at 50 rows + preserves sort order [high-risk]") {
    auto tickets = MakeTickets(60);
    const auto sortedIdx = IdentitySort(tickets->size());
    // Select all 60 sort-order rows.
    std::set<int> selected;
    for (int i = 0; i < 60; ++i) {
        selected.insert(i);
    }
    const std::string body = AiContextBuilder::BuildSelectionBody(*tickets, sortedIdx, selected);
    // Must contain exactly 50 lines (one per kept row).
    std::size_t lines = 0;
    for (char c : body) {
        if (c == '\n') {
            ++lines;
        }
    }
    CHECK(lines == 50);
    // First emitted row must be sort-order index 0 -> SMA-100; last kept row index 49 -> SMA-149.
    CHECK(Contains(body, "SMA-100"));
    CHECK(Contains(body, "SMA-149"));
    // Row 50 should NOT appear (capped).
    CHECK_FALSE(Contains(body, "SMA-150"));
    CHECK_FALSE(Contains(body, "SMA-159"));
}

TEST_CASE("AiContextBuilder::BuildSelectionBody — empty selection yields empty") {
    auto tickets = MakeTickets(5);
    const auto sortedIdx = IdentitySort(tickets->size());
    std::set<int> empty;
    CHECK(AiContextBuilder::BuildSelectionBody(*tickets, sortedIdx, empty).empty());
}

TEST_CASE("AiContextBuilder::BuildSelectionBody — out-of-range rows are silently skipped") {
    auto tickets = MakeTickets(3);
    const auto sortedIdx = IdentitySort(tickets->size());
    std::set<int> selected = {-1, 0, 1, 99, 100}; // -1 and 99/100 invalid
    const std::string body = AiContextBuilder::BuildSelectionBody(*tickets, sortedIdx, selected);
    // Only rows 0 and 1 contributed.
    CHECK(Contains(body, "SMA-100"));
    CHECK(Contains(body, "SMA-101"));
    CHECK_FALSE(Contains(body, "SMA-102"));
}

TEST_CASE("AiContextBuilder::BuildVisibleRowsBody — caps at 50 + skips out-of-range") {
    auto tickets = MakeTickets(80);
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < 100; ++i) { // includes 20 OOB indices >= tickets->size()
        visible.push_back(i);
    }
    const std::string body = AiContextBuilder::BuildVisibleRowsBody(*tickets, visible);
    std::size_t lines = 0;
    for (char c : body) {
        if (c == '\n') {
            ++lines;
        }
    }
    CHECK(lines == 50);
    CHECK(Contains(body, "SMA-100"));
    CHECK(Contains(body, "SMA-149"));
    CHECK_FALSE(Contains(body, "SMA-150"));
}

TEST_CASE("AiContextBuilder::BuildActiveTicketBody — empty active key yields empty") {
    auto tickets = MakeTickets(3);
    CHECK(AiContextBuilder::BuildActiveTicketBody(*tickets, std::string()).empty());
}

TEST_CASE("AiContextBuilder::BuildActiveTicketBody — emits id + canonical scalar fields") {
    auto tickets = MakeTickets(3);
    const std::string body = AiContextBuilder::BuildActiveTicketBody(*tickets, "SMA-101");
    CHECK(Contains(body, "id: SMA-101"));
    CHECK(Contains(body, "summary: Ticket #1"));
    CHECK(Contains(body, "status:"));
}

TEST_CASE("AiContextBuilder::BuildActiveTicketBody — unknown id returns empty") {
    auto tickets = MakeTickets(2);
    CHECK(AiContextBuilder::BuildActiveTicketBody(*tickets, "DOES-NOT-EXIST").empty());
}

TEST_CASE("AiContextBuilder::BuildActiveViewBody — null view yields empty; populated emits fields") {
    CHECK(AiContextBuilder::BuildActiveViewBody(nullptr).empty());
    ViewDefinition v;
    v.Id = "v-default";
    v.Name = "My Sprint";
    v.Jql = "project = SMA AND sprint in openSprints()";
    v.ColumnOrder = {"id", "summary", "status"};
    const std::string body = AiContextBuilder::BuildActiveViewBody(&v);
    CHECK(Contains(body, "name: My Sprint"));
    CHECK(Contains(body, "id: v-default"));
    CHECK(Contains(body, "query: project = SMA AND sprint in openSprints()"));
    CHECK(Contains(body, "columns: id summary status"));
}

TEST_CASE("AiContextBuilder::BuildAuditTrailBody — caps at 20 + emits most-recent-first") {
    std::vector<std::string> lines;
    for (int i = 0; i < 30; ++i) {
        lines.push_back("{\"i\":" + std::to_string(i) + "}");
    }
    const std::string body = AiContextBuilder::BuildAuditTrailBody(lines);
    // The 20 most-recent entries are indices 29..10. The very first emitted line must
    // be the highest index (29); the 21st input (index 9) must NOT appear.
    CHECK(Contains(body, "{\"i\":29}"));
    CHECK(Contains(body, "{\"i\":10}"));
    CHECK_FALSE(Contains(body, "{\"i\":9}"));
    std::size_t lineCount = 0;
    for (char c : body) {
        if (c == '\n') {
            ++lineCount;
        }
    }
    CHECK(lineCount == 20);
}

TEST_CASE("AiContextBuilder::BuildAll — disabled flags zero-out matching blocks [high-risk]") {
    auto tickets = MakeTickets(5);
    const auto sortedIdx = IdentitySort(tickets->size());
    std::set<int> selected = {0, 2, 4};
    ViewDefinition v;
    v.Name = "T";
    v.Jql = "true";

    AiContextBuilder::Inputs in;
    in.Tickets = tickets;
    in.SortedIndices = &sortedIdx;
    in.SelectedRows = &selected;
    in.ActiveIssueId = "SMA-101";
    in.ActiveView = &v;
    in.VisibleRows = &sortedIdx;

    // Disable Selection + ActiveView; keep VisibleRows + ActiveTicket + AuditTrail.
    in.EnableSelection = false;
    in.EnableActiveView = false;
    in.EnableAuditTrail = false; // disable to avoid pulling real audit file

    const auto blocks = AiContextBuilder::BuildAll(in);
    REQUIRE(blocks.size() == 5);
    CHECK(blocks[0].Body.empty()); // Selection — disabled
    CHECK_FALSE(blocks[1].Body.empty()); // VisibleRows — enabled
    CHECK_FALSE(blocks[2].Body.empty()); // ActiveTicket — enabled
    CHECK(blocks[3].Body.empty()); // ActiveView — disabled
    CHECK(blocks[4].Body.empty()); // AuditTrail — disabled
}

TEST_CASE("AiContextBuilder::BuildAll — ActiveTicket fast path validates pre-resolved id [high-risk]") {
    // The O(1) PreResolvedActiveTicket fast path must verify the pointer's id
    // matches ActiveIssueId. A STALE pre-resolved pointer (id != ActiveIssueId)
    // must NOT surface the wrong ticket — BuildAll must fall back to the scan and
    // resolve ActiveIssueId from the snapshot.
    auto tickets = MakeTickets(5); // SMA-100 .. SMA-104

    // Matched: pre-resolved pointer whose id equals ActiveIssueId is used directly.
    {
        AiContextBuilder::Inputs in;
        in.Tickets = tickets;
        in.ActiveIssueId = "SMA-102";
        in.PreResolvedActiveTicket = &(*tickets)[2]; // SMA-102 — matches
        in.EnableSelection = false;
        in.EnableVisibleRows = false;
        in.EnableActiveView = false;
        in.EnableAuditTrail = false;
        const auto blocks = AiContextBuilder::BuildAll(in);
        REQUIRE(blocks.size() == 5);
        CHECK(Contains(blocks[2].Body, "id: SMA-102"));
    }

    // Mismatched: pre-resolved pointer is SMA-104 but ActiveIssueId is SMA-101.
    // The guard rejects the stale pointer and the scan resolves SMA-101 instead.
    {
        AiContextBuilder::Inputs in;
        in.Tickets = tickets;
        in.ActiveIssueId = "SMA-101";
        in.PreResolvedActiveTicket = &(*tickets)[4]; // SMA-104 — mismatch
        in.EnableSelection = false;
        in.EnableVisibleRows = false;
        in.EnableActiveView = false;
        in.EnableAuditTrail = false;
        const auto blocks = AiContextBuilder::BuildAll(in);
        REQUIRE(blocks.size() == 5);
        CHECK(Contains(blocks[2].Body, "id: SMA-101"));
        CHECK_FALSE(Contains(blocks[2].Body, "id: SMA-104"));
    }
}

TEST_CASE("AiContextBuilder::MergeEnabled — wraps non-empty blocks in <smatchet_context> tags") {
    auto tickets = MakeTickets(3);
    const auto sortedIdx = IdentitySort(tickets->size());
    std::set<int> selected = {0, 1};

    AiContextBuilder::Inputs in;
    in.Tickets = tickets;
    in.SortedIndices = &sortedIdx;
    in.SelectedRows = &selected;
    in.VisibleRows = &sortedIdx;
    in.ActiveIssueId = "SMA-100";
    in.EnableActiveView = false;
    in.EnableAuditTrail = false;

    const std::string merged = AiContextBuilder::MergeEnabled(in);
    REQUIRE(!merged.empty());
    CHECK(Contains(merged, "<smatchet_context block=\"selected_tickets\">"));
    CHECK(Contains(merged, "<smatchet_context block=\"visible_rows\">"));
    CHECK(Contains(merged, "<smatchet_context block=\"active_ticket\">"));
    // Disabled blocks contribute no opening tag.
    CHECK_FALSE(Contains(merged, "<smatchet_context block=\"active_view\">"));
    CHECK_FALSE(Contains(merged, "<smatchet_context block=\"audit_trail\">"));
    // Every opening tag is matched by a closing tag.
    CHECK(Contains(merged, "</smatchet_context>"));
}
