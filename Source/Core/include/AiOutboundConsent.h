#ifndef SMATCHET_AI_OUTBOUND_CONSENT_H
#define SMATCHET_AI_OUTBOUND_CONSENT_H

// AiOutboundConsent — pure decision logic for the first-send outbound-context
// consent modal (docs/self-improvement/categories/security.md 2026-05-17 P2).
//
// Before the very first AI turn of a fresh profile, the Assistant panel raises a
// one-time modal that enumerates the `AssistantContextBlock*` blocks, their
// enabled/disabled state, and their measured payload sizes so the user can see
// exactly what leaves the machine before any provider POST. The acknowledgement
// flips `TrackerConfig::AssistantOutboundConsentShown` to true (persisted), so
// the gate fires once per profile, not per turn.
//
// Everything here is header-inline pure logic — no AppController / cpr / ImGui
// coupling — so the doctest rig (tests/Core/AiOutboundConsent.test.cpp) can pin
// every branch without the UI TU's dependency chain. Mirrors the extraction
// pattern used by `DecideSendDisabled` (AiAssistantSendButton.h) and
// `ComposeSystemPrompt` (AiAssistantController.h). The UI layer measures the
// real per-block body sizes and feeds them in; the rows it gets back drive the
// modal's display verbatim.

#include <cstddef>
#include <string>
#include <vector>

namespace smatchet {
namespace ai {

/// Gate predicate read by the Assistant send path. Returns true iff the one-time
/// outbound-context consent modal must be shown before this turn is dispatched.
/// `consentAlreadyShown` is `TrackerConfig::AssistantOutboundConsentShown`: false
/// for a fresh profile (gate fires), true once the user has acknowledged (gate
/// never fires again). Trivial by construction — extracted so the "fires exactly
/// once, defaults to fire" contract is pinned by a test and cannot silently
/// invert to "default to send" under a future refactor.
inline bool ShouldRequireOutboundConsent(bool consentAlreadyShown) {
    return !consentAlreadyShown;
}

/// One display row in the consent modal — a single outbound context block.
struct OutboundConsentBlockRow {
    /// Canonical block name as emitted in the `<smatchet_context block="...">`
    /// wrapper (matches AiContextBuilder::BlockName): e.g. "selected_tickets".
    std::string Name;
    /// Whether the matching `cfg.AssistantContextBlock*` toggle is on. A disabled
    /// block contributes nothing to the outbound payload.
    bool Enabled;
    /// Measured body size in bytes for an enabled, non-deferred block. Zero when
    /// the block is disabled or its fetch is deferred (see DeferredFetch).
    std::size_t ApproxBytes;
    /// True for the audit-trail block: its body is fetched on the worker thread at
    /// send time (UX-pillar-2 deferral), so its exact size is unknown when the
    /// modal renders. The modal labels it "fetched at send" instead of a size.
    bool DeferredFetch;

    OutboundConsentBlockRow() : Enabled(false), ApproxBytes(0), DeferredFetch(false) {}
    OutboundConsentBlockRow(std::string name, bool enabled, std::size_t approxBytes, bool deferred)
        : Name(std::move(name)), Enabled(enabled), ApproxBytes(approxBytes), DeferredFetch(deferred) {}
};

/// Measured byte sizes + enable flags for the four eagerly-built context blocks,
/// plus the audit-trail toggle. The UI builds the same context snapshot the next
/// turn would send (with the audit-trail fetch deferred) and passes the measured
/// body sizes here. Defaults keep the struct a valid empty snapshot.
struct OutboundConsentInputs {
    bool EnableSelection = false;
    bool EnableVisibleRows = false;
    bool EnableActiveTicket = false;
    bool EnableActiveView = false;
    bool EnableAuditTrail = false;
    std::size_t SelectionBytes = 0;
    std::size_t VisibleRowsBytes = 0;
    std::size_t ActiveTicketBytes = 0;
    std::size_t ActiveViewBytes = 0;
};

/// Build the ordered modal rows from the enable flags + measured sizes. Order is
/// fixed and matches the on-the-wire block order (selected_tickets, visible_rows,
/// active_ticket, active_view, audit_trail) so the modal reads in the same order
/// the provider receives them. The audit-trail row is always present and always
/// DeferredFetch — its size is only known on the worker thread at send time.
/// A disabled block still gets a row (so the user sees what is off), with
/// ApproxBytes forced to 0 regardless of any measured value.
inline std::vector<OutboundConsentBlockRow> BuildOutboundConsentRows(const OutboundConsentInputs& in) {
    std::vector<OutboundConsentBlockRow> rows;
    rows.reserve(5);
    rows.emplace_back("selected_tickets", in.EnableSelection,
                      in.EnableSelection ? in.SelectionBytes : 0, false);
    rows.emplace_back("visible_rows", in.EnableVisibleRows,
                      in.EnableVisibleRows ? in.VisibleRowsBytes : 0, false);
    rows.emplace_back("active_ticket", in.EnableActiveTicket,
                      in.EnableActiveTicket ? in.ActiveTicketBytes : 0, false);
    rows.emplace_back("active_view", in.EnableActiveView,
                      in.EnableActiveView ? in.ActiveViewBytes : 0, false);
    // Audit trail: deferred fetch, so no measurable size at modal time.
    rows.emplace_back("audit_trail", in.EnableAuditTrail, 0, true);
    return rows;
}

/// Sum of the bytes the user can see up-front: enabled, non-deferred blocks only.
/// The deferred audit-trail block is excluded (its size is unknown here) — the
/// modal surfaces that exclusion in its total line so the number is honest.
inline std::size_t SumOutboundConsentBytes(const std::vector<OutboundConsentBlockRow>& rows) {
    std::size_t total = 0;
    for (std::vector<OutboundConsentBlockRow>::const_iterator it = rows.begin(); it != rows.end(); ++it) {
        if (it->Enabled && !it->DeferredFetch) {
            total += it->ApproxBytes;
        }
    }
    return total;
}

} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_OUTBOUND_CONSENT_H
