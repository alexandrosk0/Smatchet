// AiOutboundConsent — unit gate for the first-send outbound-context consent
// modal (docs/self-improvement/categories/security.md 2026-05-17 P2). The modal
// is the one-time disclosure of what workspace context leaves the machine on the
// first AI turn of a fresh profile. The three pinned contracts:
//
//   - ShouldRequireOutboundConsent defaults to FIRE (true) and flips to
//     never-again (false) only once consent is recorded — it must not silently
//     invert to "default to send" under a refactor.
//   - BuildOutboundConsentRows emits all five blocks in on-the-wire order, marks
//     disabled blocks as 0-byte, and always flags audit-trail as deferred.
//   - SumOutboundConsentBytes counts only what the user can see up-front
//     (enabled, non-deferred), so the modal's total is honest.
//
// See Source/Core/include/AiOutboundConsent.h for the helpers + rationale.

#include <doctest/doctest.h>

#include "AiOutboundConsent.h"

using smatchet::ai::BuildOutboundConsentRows;
using smatchet::ai::OutboundConsentBlockRow;
using smatchet::ai::OutboundConsentInputs;
using smatchet::ai::ShouldRequireOutboundConsent;
using smatchet::ai::SumOutboundConsentBytes;

TEST_CASE("ShouldRequireOutboundConsent: fresh profile fires the gate") {
    // AssistantOutboundConsentShown defaults false on a fresh profile — the
    // modal MUST show before the first outbound turn.
    CHECK(ShouldRequireOutboundConsent(/*consentAlreadyShown=*/false));
}

TEST_CASE("ShouldRequireOutboundConsent: acknowledged profile never fires again") {
    // Once the user has acknowledged (latch persisted true), the gate is
    // permanently silent — consent is one-time, not per-turn.
    CHECK_FALSE(ShouldRequireOutboundConsent(/*consentAlreadyShown=*/true));
}

TEST_CASE("BuildOutboundConsentRows: five blocks in on-the-wire order") {
    OutboundConsentInputs in;
    in.EnableSelection = in.EnableVisibleRows = in.EnableActiveTicket = in.EnableActiveView =
        in.EnableAuditTrail = true;
    const std::vector<OutboundConsentBlockRow> rows = BuildOutboundConsentRows(in);
    REQUIRE(rows.size() == 5u);
    CHECK(rows[0].Name == "selected_tickets");
    CHECK(rows[1].Name == "visible_rows");
    CHECK(rows[2].Name == "active_ticket");
    CHECK(rows[3].Name == "active_view");
    CHECK(rows[4].Name == "audit_trail");
}

TEST_CASE("BuildOutboundConsentRows: audit-trail is always deferred, others are not") {
    OutboundConsentInputs in;
    in.EnableAuditTrail = true;
    const std::vector<OutboundConsentBlockRow> rows = BuildOutboundConsentRows(in);
    CHECK_FALSE(rows[0].DeferredFetch);
    CHECK_FALSE(rows[1].DeferredFetch);
    CHECK_FALSE(rows[2].DeferredFetch);
    CHECK_FALSE(rows[3].DeferredFetch);
    CHECK(rows[4].DeferredFetch);
    // Deferred row never carries a measured size even when enabled.
    CHECK(rows[4].Enabled);
    CHECK(rows[4].ApproxBytes == 0u);
}

TEST_CASE("BuildOutboundConsentRows: measured sizes flow through for enabled blocks") {
    OutboundConsentInputs in;
    in.EnableSelection = true;
    in.EnableVisibleRows = true;
    in.SelectionBytes = 128;
    in.VisibleRowsBytes = 4096;
    const std::vector<OutboundConsentBlockRow> rows = BuildOutboundConsentRows(in);
    CHECK(rows[0].Enabled);
    CHECK(rows[0].ApproxBytes == 128u);
    CHECK(rows[1].Enabled);
    CHECK(rows[1].ApproxBytes == 4096u);
}

TEST_CASE("BuildOutboundConsentRows: disabled block is zeroed even with a stray size") {
    // A disabled toggle contributes nothing on the wire — its row must read 0
    // bytes regardless of any measured value the caller happens to pass.
    OutboundConsentInputs in;
    in.EnableActiveTicket = false;
    in.ActiveTicketBytes = 9999;
    const std::vector<OutboundConsentBlockRow> rows = BuildOutboundConsentRows(in);
    CHECK_FALSE(rows[2].Enabled);
    CHECK(rows[2].ApproxBytes == 0u);
}

TEST_CASE("SumOutboundConsentBytes: counts only enabled, non-deferred blocks") {
    OutboundConsentInputs in;
    in.EnableSelection = true;
    in.EnableVisibleRows = true;
    in.EnableActiveTicket = false; // off — excluded
    in.EnableActiveView = true;
    in.EnableAuditTrail = true; // deferred — excluded
    in.SelectionBytes = 100;
    in.VisibleRowsBytes = 200;
    in.ActiveTicketBytes = 400; // off, must not count
    in.ActiveViewBytes = 50;
    const std::vector<OutboundConsentBlockRow> rows = BuildOutboundConsentRows(in);
    // 100 + 200 + 50 = 350; active_ticket (off) and audit_trail (deferred) excluded.
    CHECK(SumOutboundConsentBytes(rows) == 350u);
}

TEST_CASE("SumOutboundConsentBytes: all-off totals zero") {
    OutboundConsentInputs in; // every flag false by default
    CHECK(SumOutboundConsentBytes(BuildOutboundConsentRows(in)) == 0u);
}
