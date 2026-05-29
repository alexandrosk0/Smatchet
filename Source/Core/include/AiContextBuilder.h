#ifndef SMATCHET_AI_CONTEXT_BUILDER_H
#define SMATCHET_AI_CONTEXT_BUILDER_H

#include "AiTypes.h"
#include "CachedTicketTypes.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct ViewDefinition;

/// Snapshot-builder for the 5 auto-context blocks injected into the AI system prompt.
///
/// Threading: **UI-thread only**. Reads from `AppController::GetActiveTicketsSnapshot()`,
/// `SpreadsheetState::RectSel.Rows`, `SpreadsheetState::ActiveIssueId`, the cached sort
/// order, and the active view definition. These all live on UI-owned state. The
/// builder takes a const snapshot once and hands the resulting `std::vector<AiContextBlock>`
/// to the worker via the controller's queue, which is the threading-safe hand-off
/// (Phase B's existing `Submit` contract).
///
/// Per-block enable flags come from the 5 `cfg.AssistantContextBlock*` toggles already
/// shipped in Phase A'. The builder skips disabled blocks (empty body) but still emits
/// the entry so the caller can inspect by `AiContextBlockKind` if needed.
namespace AiContextBuilder {

/// Selection / VisibleRows cap. Plan-locked at N=50.
constexpr std::size_t kRowsCap = 50u;
/// AuditTrail cap. Plan-locked at N=20.
constexpr std::size_t kAuditCap = 20u;

/// Sentinel body inserted into the AuditTrail block when the UI-thread caller
/// asks BuildAll to defer the actual SQLite + filesystem read to a worker
/// thread (see `Inputs::DeferAuditTrailFetch`). The worker (today:
/// `AiAssistantController::RunRequest`) recognises this exact string and
/// rewrites the block body with the result of `BuildAuditTrailBody` before
/// the request is sent. Chosen to be highly unlikely to collide with any
/// real audit-trail content.
constexpr const char* kDeferredAuditTrailSentinel = "__SMATCHET_DEFERRED__";

struct Inputs {
    /// Active tickets snapshot (typically obtained via
    /// `AppController::GetActiveTicketsSnapshot()` on the UI thread). May be null in
    /// pure-logic tests — the builder degrades gracefully and emits empty bodies for
    /// ticket-dependent blocks when the snapshot is unavailable.
    std::shared_ptr<const std::vector<CachedTicket>> Tickets;
    /// Sort-order indices into the active ticket snapshot (UI-side cache). Used by the
    /// Selection + VisibleRows blocks to map `RectSel.Rows` (sort-order indices) back
    /// to the underlying CachedTicket entries.
    const std::vector<std::size_t>* SortedIndices = nullptr;
    /// Whole-row selection set from the grid. Sort-order indices.
    const std::set<int>* SelectedRows = nullptr;
    /// Currently-active ticket id (one of the `CachedTicket::id` values). Empty means
    /// "no ticket focused" — the ActiveTicket block emits an empty body in that case.
    std::string ActiveIssueId;
    /// Snapshot of the currently-active view. May be null when no view is active.
    const ViewDefinition* ActiveView = nullptr;
    /// First N rows of the visible-rows cap mapped to the same sort-order indices.
    /// Pass `SortedIndices` again if you want all sorted rows; the builder caps
    /// internally at `kRowsCap` rows.
    const std::vector<std::size_t>* VisibleRows = nullptr;
    /// Per-block enable flags (mirrors the 5 `cfg.AssistantContextBlock*` bools).
    bool EnableSelection = true;
    bool EnableVisibleRows = true;
    bool EnableActiveTicket = true;
    bool EnableActiveView = true;
    bool EnableAuditTrail = true;
    /// When true, the AuditTrail block body is set to `kDeferredAuditTrailSentinel`
    /// instead of the full audit dump — the caller commits to fetching + replacing
    /// the body on a worker thread before the request is dispatched. UI-thread
    /// callers MUST pass `true` to honour UX pillar 2 (no synchronous filesystem
    /// / SQLite read reaches the UI frame). Pure-logic tests default to `false`
    /// for backwards compatibility with the Phase C test rig.
    bool DeferAuditTrailFetch = false;
};

/// Build all 5 blocks. Disabled blocks have empty Body (kind + name still set).
/// Block order in result vector matches `AiContextBlockKind` ordinal: Selection (0),
/// VisibleRows (1), ActiveTicket (2), ActiveView (3), AuditTrail (4).
std::vector<AiContextBlock> BuildAll(const Inputs& in);

/// Convenience: BuildAll + concatenate enabled non-empty bodies with section separators
/// into one string. Empty if no block has content. Format per block:
///
///     <smatchet_context block="<name>">
///     <body>
///     </smatchet_context>
///
/// Sections are joined with single `\n` between closing/opening tags.
std::string MergeEnabled(const Inputs& in);

// ---- Pure helpers exposed for tests (no AppController coupling) ----------

/// Render a single block's body for the Selection block: at most `kRowsCap` rows,
/// dereferenced via the provided tickets list + sort-order indices.
std::string BuildSelectionBody(const std::vector<CachedTicket>& tickets, const std::vector<std::size_t>& sortedIndices,
                               const std::set<int>& selectedRows);

/// Render the VisibleRows block body.
std::string BuildVisibleRowsBody(const std::vector<CachedTicket>& tickets, const std::vector<std::size_t>& visibleRows);

/// Render the ActiveTicket block body. Empty `activeIssueId` returns empty string.
std::string BuildActiveTicketBody(const std::vector<CachedTicket>& tickets, const std::string& activeIssueId);

/// Render the ActiveView block body. Null view returns empty string.
std::string BuildActiveViewBody(const ViewDefinition* view);

/// Render the AuditTrail block body from a pre-collected JSON-event list (each
/// element is the JSON-encoded payload from `BackendAuditTrail::ReadRecentEvents`).
std::string BuildAuditTrailBody(const std::vector<std::string>& recentJsonLines);

} // namespace AiContextBuilder

#endif
