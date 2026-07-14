# Rich text editing v2 — remaining backlog
<!-- index-summary: Rich-text editing v2 backlog — golden / snapshot tests for `MarkdownToAdf` / `AdfToMarkdown` / `MarkdownToHtml` / `HtmlSubsetToMarkdown`; raw-mode + fidelity UX. Originally `backlog/RICH_TEXT_EDITING_V2_REMAINING.md`. -->

Tracks gaps in the rich-text-editing v2 work after the core grid pipeline landed (`MarkdownConvert`, md4c, `fieldRichValues`, long-text modal, offline 3-way merge, conflict modal). The original `RICH_TEXT_EDITING_V2_PLAN.md` design doc predates this repo's `docs/plans/` tree and was never migrated in; this file is the surviving canonical tracker for the v2 subset and its open items.

> **Status (2026-07-14 audit + slice):** the *Raw-mode and fidelity UX* section is now **done** (read-only source pane, Save acknowledgement gate, on-save loss toast); the *Cache / submit behavior* item (untouched rich fields) is **done by architecture**; the converter-goldens item is **partial** (`MarkdownToHtml` golden coverage added; real-sample fixture capture still open). The *Field coverage and surfaces* items (comments, worklog, bulk import, new-issue draft) remain open — each is a multi-surface change with a product/UX decision (whether those surfaces adopt Markdown semantics) and needs UI verification, so they were deliberately left for a follow-up. Section markers updated inline.

## Testing and quality

- **Converter goldens — PARTIAL.** Inline golden/snapshot tests exist for `HtmlSubsetToMarkdown` and `MarkdownToHtml` ([`tests/Core/MarkdownConvert.test.cpp`](../../../tests/Core/MarkdownConvert.test.cpp)) and for `MarkdownToAdf` / `AdfToMarkdown` ([`tests/Core/MarkdownConvertAdf.test.cpp`](../../../tests/Core/MarkdownConvertAdf.test.cpp)). **Still open:** capture real Jira ADF and Plane `description_html` samples as external **fixture documents** (today's goldens are inline string literals, not captured real-world payloads).
- **Regression:** Re-run or extend tests after md4c bumps or flag changes (`Md4cParserFlags()`).

## Raw-mode and fidelity UX (plan §103–113) — DONE

- **Read-only source pane — DONE.** `TicketFieldEditor_Modal.cpp` `DrawLongTextSourcePane()` shows the original **ADF JSON** (re-indented) or **HTML** in a collapsible read-only pane above the editor on high-risk paths (raw-HTML fallback, or an ADF doc with dropped nodes).
- **Acknowledgement — DONE.** `DrawLongTextFooter()` disables **Save** behind an **“I understand — save anyway”** checkbox whenever the save would drop constructs (truncation, dropped ADF nodes, or a lossy round-trip). Decision logic is the unit-tested `TicketFieldEditorLongTextPure::AssessLongTextSaveFidelity`.
- **Toasts — DONE.** `CommitLongTextEdit()` raises a **Warning** toast on save summarizing what will be lost / stored best-effort (raw-HTML verbatim warns but does not block the save).

## Server errors (plan §92–98)

- **Plane:** Align failure `outError` with Jira-style **truncated response body** where useful (`PlaneClient.cpp` already surfaces `response.text` / parsed fields; optional: same `TruncateForLog`-style cap as Jira for very large bodies).

## Field coverage and surfaces (plan §5–6)

- **Comments:** Same Markdown ↔ ADF fidelity as grid long-text (today comment paths may still use plain paragraph ADF helpers).
- **Worklog descriptions:** Decide migrate to **modal** vs keep small inline multiline; if modal, share the grid long-text helper.
- **Plane:** Audit **non-description** multiline fields in `PlaneClient.cpp` and route like description where applicable.
- **Bulk import:** [`SmatchetBulkTicketsUi.cpp`](../../../Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp) — wire **Markdown + converters** for description-like fields (plan PR-G).
- **New issue draft:** [`SmatchetNewIssueDraftUi.cpp`](../../../Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp) — optional: reuse the **same modal component** as the grid for consistency (today description is **inline** multiline with Markdown hint).

## Cache / submit behavior (plan §2) — DONE

- **Untouched rich fields:** Verified done by architecture. Long-text (modal) edits create a `PendingFieldEdit` only when the buffer actually changed (`CommitLongTextEdit`, gated by `ShouldQueueLongTextEdit`), and payloads are built per-edited-field (`BuildAdfScalar` runs `MarkdownToAdf` only on the edited scalar). A rich field the user never opened is never added to `pendingEdits`, so it is not re-emitted on unrelated ticket updates — no "emit all fields" path re-serializes untouched rich values. No further work needed unless a bulk path starts round-tripping whole tickets.

## Explicitly out of v2 (keep in v3+ backlog)

- Image/attachment paste in descriptions.
- Custom Markdown for **mentions** / smart links.
- Full **WYSIWYG** rendered editor.
- **Tier 2** full HTML5 parser (only if Tier 1 raw-mode fires often on real Plane HTML).

## References

- Implementation touchpoints: `MarkdownConvert.{h,cpp}`, `TicketFieldEditor.cpp`, `TicketFieldEditor_Modal.cpp`, `LocalCacheManager.{h,cpp}`, `JiraIssueSearch.cpp`, `JiraIssueMutation.cpp`, `PlaneClient.cpp`, `TrackerFieldPayloadPure.cpp`, `AppController_IssueCreateOffline.cpp`, `TextMerge.{h,cpp}`, `SmatchetOfflineQueueUi.cpp`.
