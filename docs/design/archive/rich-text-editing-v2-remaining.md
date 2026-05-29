# Rich text editing v2 — remaining backlog
<!-- index-summary: Rich-text editing v2 backlog — golden / snapshot tests for `MarkdownToAdf` / `AdfToMarkdown` / `MarkdownToHtml` / `HtmlSubsetToMarkdown`; raw-mode + fidelity UX. Originally `backlog/RICH_TEXT_EDITING_V2_REMAINING.md`. -->

Tracks gaps versus [`RICH_TEXT_EDITING_V2_PLAN.md`](../RICH_TEXT_EDITING_V2_PLAN.md) after the core grid pipeline landed (`MarkdownConvert`, md4c, `fieldRichValues`, long-text modal, offline 3-way merge, conflict modal).

## Testing and quality

- **PR-A style:** Unit tests against **fixture documents** for `MarkdownToAdf` / `AdfToMarkdown` / `MarkdownToHtml` / `HtmlSubsetToMarkdown` (golden or snapshot tests). Capture real Jira ADF and Plane `description_html` samples over time.
- **Regression:** Re-run or extend tests after md4c bumps or flag changes (`Md4cParserFlags()`).

## Raw-mode and fidelity UX (plan §103–113)

- **Read-only source pane:** Show original **ADF JSON** or **HTML** in a read-only code area above the editor when in raw / high-risk paths (today raw mode is mainly **HTML in the same buffer**; dropped ADF nodes only get a **warning banner**, not a JSON preview).
- **Acknowledgement:** **Disable Save** until the user confirms **“I understand”** (or equivalent) when formatting loss is possible.
- **Toasts:** On save, **warn** if constructs were dropped or if save is best-effort (plan mentions toast when something was dropped).

## Server errors (plan §92–98)

- **Plane:** Align failure `outError` with Jira-style **truncated response body** where useful (`PlaneClient.cpp` already surfaces `response.text` / parsed fields; optional: same `TruncateForLog`-style cap as Jira for very large bodies).

## Field coverage and surfaces (plan §5–6)

- **Comments:** Same Markdown ↔ ADF fidelity as grid long-text (today comment paths may still use plain paragraph ADF helpers).
- **Worklog descriptions:** Decide migrate to **modal** vs keep small inline multiline; if modal, share the grid long-text helper.
- **Plane:** Audit **non-description** multiline fields in `PlaneClient.cpp` and route like description where applicable.
- **Bulk import:** [`SmatchetBulkTicketsUi.cpp`](../Source_Core/src/SmatchetBulkTicketsUi.cpp) — wire **Markdown + converters** for description-like fields (plan PR-G).
- **New issue draft:** [`SmatchetNewIssueDraftUi.cpp`](../Source_Core/src/SmatchetNewIssueDraftUi.cpp) — optional: reuse the **same modal component** as the grid for consistency (today description is **inline** multiline with Markdown hint).

## Cache / submit behavior (plan §2)

- **Untouched rich fields:** Audit update/submit paths so a field the user **never opened** in the modal is **not re-emitted** on unrelated ticket updates (silent formatting loss). Verify end-to-end with grid edits and bulk operations.

## Explicitly out of v2 (keep in v3+ backlog)

- Image/attachment paste in descriptions.
- Custom Markdown for **mentions** / smart links.
- Full **WYSIWYG** rendered editor.
- **Tier 2** full HTML5 parser (only if Tier 1 raw-mode fires often on real Plane HTML).

## References

- Implementation touchpoints: `MarkdownConvert.{h,cpp}`, `TicketFieldEditor.cpp`, `LocalCacheManager.{h,cpp}`, `JiraIssueSearch.cpp`, `PlaneClient.cpp`, `TrackerFieldPayload.cpp`, `AppController_IssueCreateOffline.cpp`, `TextMerge.{h,cpp}`, `SmatchetOfflineQueueUi.cpp`.
