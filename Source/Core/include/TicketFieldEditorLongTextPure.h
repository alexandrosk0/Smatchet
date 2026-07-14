#ifndef SMATCHET_TICKET_FIELD_EDITOR_LONG_TEXT_PURE_H
#define SMATCHET_TICKET_FIELD_EDITOR_LONG_TEXT_PURE_H

#include <string>
#include <vector>

/**
 * Pure-logic helpers lifted from TicketFieldEditor.cpp's anonymous namespace so the
 * long-text rich-payload classification + seed computation can be unit-tested without
 * standing up ImGui / AppController / MainThreadDispatcher.
 *
 * Implementations are byte-identical to the originals; only namespace + linkage changes.
 * MarkdownConvert is the sole dependency (also pure — no ImGui, no I/O).
 *
 * Owner: TicketFieldEditor (UI) — pure helpers live here.
 * Test surface: tests/Core/TicketFieldEditorLongTextPure.test.cpp.
 */
namespace TicketFieldEditorLongTextPure {

/// Source format of the original rich payload. Determines which converter seeds the Markdown
/// buffer on open and which target format is expected by the payload layer.
enum class LongTextRichKind { None, Adf, Html };

/**
 * Determine whether a stored rich payload looks like ADF JSON or HTML. Returns None when the
 * input is empty or unrecognizable (caller falls back to the stripped text). Cheap leading
 * whitespace skip; first non-space byte decides: `{` triggers a non-throwing JSON parse to
 * confirm `type == "doc"`, `<` is treated as HTML on inspection-only basis. Worst case is
 * one short JSON parse on the leading payload — milliseconds.
 */
LongTextRichKind ClassifyRichValue(const std::string& rich);

/**
 * Compute the Markdown seed (and capture the dropped-ADF-nodes side-channel) from a rich
 * value. Pure function — no UI access. Safe to call on a worker thread.
 *
 * - Adf: nlohmann::json::parse + MarkdownConvert::AdfToMarkdown. On exception (malformed
 *   JSON) falls back to `strippedFallback`.
 * - Html: MarkdownConvert::HtmlSubsetToMarkdown. When the subset converter falls back
 *   (unknown tags), sets `outRawMode = true` and returns the raw HTML so the editor can
 *   surface it unmodified.
 * - None: returns `strippedFallback` verbatim.
 *
 * `outDroppedAdfNodeTypes` is cleared on entry and populated only on the Adf branch.
 * `outRawMode` is set to false on entry and only flipped on the Html-fallback branch.
 */
std::string ComputeLongTextSeed(LongTextRichKind kind, const std::string& rich, const std::string& strippedFallback,
                                std::vector<std::string>& outDroppedAdfNodeTypes, bool& outRawMode);

/// Result of the modal's preview round-trip: the re-rendered Markdown plus whether the
/// round-trip dropped constructs (HTML-subset fallback, or non-empty ADF dropped-nodes list).
struct RoundTripPreview {
    std::string Rendered;
    bool Lossy = false;
};

/// Round-trips the in-progress Markdown buffer through the same converter the modal runs on save
/// (Markdown to ADF / HTML and back) so conversion loss surfaces in the preview before save. Pure
/// function with no UI access, safe to unit-test. The Html kind goes Markdown-to-HTML then through
/// the HTML-subset converter, mirroring its subset-fallback flag into Lossy. The None and Adf kinds
/// go Markdown-to-ADF and back, marking Lossy when ADF reports dropped nodes. Any converter
/// exception falls back to the raw Markdown with Lossy false, matching the modal's in-progress-edit
/// guard where mid-token input occasionally trips the converter.
RoundTripPreview ComputeRoundTripPreview(LongTextRichKind kind, const std::string& markdown);

/// Rich description payload (Jira ADF JSON or Plane HTML) → markdown for grid tooltips.
/// Falls back to `strippedFallback` when empty, plain, or HTML outside the supported subset.
std::string RichValueToTooltipMarkdown(const std::string& rich, const std::string& strippedFallback);

/// What SeedLongTextBuffer (TicketFieldEditor_Modal.cpp) actually loads into the fixed-size
/// edit buffer: the seed's first `bufferCapacity - 1` bytes plus whether anything was cut.
struct LongTextSeedPlan {
    std::string Shown;
    bool Truncated = false;
};

/**
 * Plan the copy of `seed` into an edit buffer of `bufferCapacity` bytes (one reserved for the
 * NUL terminator). `Shown` is the exact string the editor displays and MUST be the Save-diff
 * baseline: diffing the buffer against the untruncated seed instead makes an unmodified
 * over-limit document always look "changed", queuing a save that overwrites the tracker field
 * with truncated text purely because it was opened and closed — CPP_CODE_AUDIT.md #1.
 * `bufferCapacity == 0` yields an empty plan.
 */
LongTextSeedPlan PlanSeedCopy(const std::string& seed, std::size_t bufferCapacity);

/// True when the Save action should queue a PendingFieldEdit: the edited buffer differs from
/// `shownSeed` (the LongTextSeedPlan::Shown actually loaded, NOT the untruncated original).
/// A no-op Save on a truncated seed must never PUT — CPP_CODE_AUDIT.md #1.
bool ShouldQueueLongTextEdit(const std::string& newValue, const std::string& shownSeed);

/// Verdict on whether saving the long-text buffer may lose or best-effort the stored value,
/// aggregated from the modal's four fidelity signals so the warn/acknowledge policy is
/// unit-tested and identical across surfaces.
struct LongTextSaveFidelity {
    /// True when Save may drop constructs or store best-effort (drives the on-save warning toast).
    bool LossPossible = false;
    /// True when the modal should gate Save behind an explicit "I understand" acknowledgement.
    /// Set only for definite structural loss (truncation, dropped ADF nodes, a detected lossy
    /// round-trip) — NOT for raw-HTML verbatim saves, which warn but never block.
    bool RequireAck = false;
    /// One-line, human-readable summary of every applicable reason (empty when LossPossible is
    /// false). Suitable for the toast message and the acknowledgement tooltip.
    std::string ToastSummary;
};

/// Assess long-text Save fidelity from the modal's signals:
///   - `seedTruncated`        — the document exceeded the edit buffer; the tail was never loaded.
///   - `droppedAdfNodeTypes`  — ADF node types AdfToMarkdown could not represent (dropped on save).
///   - `roundTripLossy`       — the Markdown→rich→Markdown round-trip changed the document.
///   - `rawMode`              — editing raw HTML that will be stored verbatim (best-effort).
/// The first three imply definite loss and set RequireAck; rawMode only contributes a best-effort
/// warning. Reasons are joined (in the above precedence) into ToastSummary. Pure — no UI access.
LongTextSaveFidelity AssessLongTextSaveFidelity(bool rawMode, const std::vector<std::string>& droppedAdfNodeTypes,
                                                bool roundTripLossy, bool seedTruncated);

} // namespace TicketFieldEditorLongTextPure

#endif
