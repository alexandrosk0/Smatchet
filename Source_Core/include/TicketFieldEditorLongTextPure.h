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
 * Test surface: tests/Source_Core/TicketFieldEditorLongTextPure.test.cpp.
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

} // namespace TicketFieldEditorLongTextPure

#endif
