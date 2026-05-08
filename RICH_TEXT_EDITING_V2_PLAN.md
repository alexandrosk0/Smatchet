# Rich-Text Field Editing — v2 Plan

This document captures everything intentionally **deferred** from the v1
multiline-editing change. v1 ships a modal multiline editor for the obvious
description-class fields with plain-text round-trip (the same fidelity as today —
just with paragraph breaks now possible). v2 closes the format-fidelity gap and
expands coverage.

A future agent should read this top-to-bottom before touching `TicketFieldEditor`,
`TrackerFieldPayload`, `JiraIssueSearch`/`JiraIssueMutation`, or `PlaneClient`.

---

## Background — what v1 ships

- A modal multiline editor (`InputTextMultiline` inside an ImGui popup modal)
  opens for any field that `TrackerFieldPayload::FieldUsesAdfDocument` returns
  true for, plus Plane's `description`. Triggered from the grid edit path in
  `TicketFieldEditor::RenderTextEditor`.
- Editing surface: **plain text only**. No Markdown awareness, no ADF awareness.
  We read the same stripped string we read today, the user edits it, we write
  it back as a single-paragraph ADF / single-`<p>` HTML wrapper.
- Round-trip is still lossy (any rich formatting in the source is destroyed on
  save) — same as today. v1 only fixes the "can't add a newline" problem.
- All other field families (single-line text, dates, selects, etc.) are
  unchanged.

## Background — what v1 does NOT ship

The rest of this document.

---

## v2 scope

### 1. Format fidelity via Markdown round-trip

The hard part. Goal: a user opens the editor, sees Markdown rendered from the
original ADF/HTML, edits it, and on save we emit ADF/HTML that preserves
everything we showed them — including content they didn't touch.

**Architecture:**

- New module `Source_Core/src/MarkdownConvert.{h,cpp}` (suggested location).
- Four conversion functions:
  - `MarkdownToAdf(const std::string& md) -> nlohmann::json`
  - `AdfToMarkdown(const nlohmann::json& adf) -> std::string`
  - `MarkdownToHtml(const std::string& md) -> std::string`
  - `HtmlToMarkdown(const std::string& html) -> std::string` (subset, see below)

**Markdown parsing: md4c.**

- Vendor [md4c](https://github.com/mity/md4c) under `ThirdParty/md4c/`. Pure C,
  MIT, ~3K LoC, builds on MSVC without configuration. We only need `md4c.c`,
  `md4c.h`, and the GFM-flavor flags (no need for the HTML output module —
  we emit ADF/HTML ourselves from the parser callbacks).
- md4c is SAX-style — block_open / block_close / span_open / span_close / text
  callbacks. Our visitor maintains a node stack; for ADF, push a JSON object
  on block_open, populate `content[]` as children appear, pop on block_close.
- Enable flags: `MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
  MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_PERMISSIVEURLAUTOLINKS |
  MD_FLAG_NOHTML` (we don't want raw HTML pass-through; users edit Markdown).
- CMake integration: add a small `add_library(md4c STATIC ThirdParty/md4c/md4c.c)`
  target and link from `Source_Core`. Confirm MSVC `/W4` is clean.

**ADF → Markdown** (no library; walk the JSON we already parse):

- ADF top-level: `{type: "doc", version: 1, content: [...]}`.
- Block types to handle: `paragraph`, `heading` (level 1–6), `bulletList` /
  `orderedList` / `listItem`, `codeBlock` (with `attrs.language`), `blockquote`,
  `rule` (horizontal rule), `hardBreak`.
- Inline marks: `strong`, `em`, `code`, `link` (`attrs.href`), `strike`.
- Recursive walker, ~200 LoC.

**HTML → Markdown** (Plane's `description_html`):

- Two-tier approach:
  - **Tier 1 (cheap, v2 default):** state-machine over the tag subset Plane
    actually emits: `<p>`, `<h1>`–`<h6>`, `<ul>`, `<ol>`, `<li>`, `<strong>`,
    `<em>`, `<b>`, `<i>`, `<code>`, `<pre>`, `<a href>`, `<br>`, `<hr>`,
    `<blockquote>`, `<s>`, `<del>`. Decode entities (`&amp;`, `&lt;`, `&gt;`,
    `&quot;`, `&#39;`, numeric refs). Anything outside the allowlist trips
    raw-mode (see below).
  - **Tier 2 (deferred):** vendor a real HTML5 parser only if Tier 1 trips
    raw-mode for real Plane content in practice.
- Implementation: ~300 LoC state machine. Keep tests with fixtures captured
  from real Plane responses.

**Raw-mode fallback.**

When the source document contains constructs we can't represent in our
Markdown subset (Jira ADF panels, info-boxes, mentions, smart links,
embedded media, unknown HTML tags, etc.), the modal:

- Shows the raw ADF JSON or HTML in a read-only code area at the top.
- Shows "Editing raw — formatting may be lost on save" banner.
- Save button is disabled until the user toggles "I understand."
- On save, we still re-emit through our converters (best-effort) and warn
  in toast if anything was dropped.

### 2. Original-document preservation in `CachedTicket`

The wire-loss problem (today: read stripped → write fresh wrapper, formatting
on the server is destroyed on every round-trip) requires keeping the original
rich payload.

**Schema change:**

```cpp
struct CachedTicket {
    // ... existing ...
    std::map<std::string, std::string> fieldValues;        // stripped (display)
    std::map<std::string, std::string> fieldRichValues;    // NEW: original ADF JSON or HTML, by field id
};
```

**Population:**

- `JiraIssueSearch` (Jira ADF parse): when reading `description` /
  `environment` / any field where `FieldUsesAdfDocument` is true, store the
  raw `content` object as JSON-stringified blob in `fieldRichValues[fieldId]`
  *in addition to* the stripped text in `fieldValues[fieldId]`.
- `PlaneClient` similar: today we store `description_stripped` in
  `fieldValues["description"]`. Also store `description_html` in
  `fieldRichValues["description"]`.
- Persist in `IssueTableSerializer` (cache I/O) so offline reload preserves
  fidelity.

**Behavior:**

- On modal-edit-open: if `fieldRichValues[fieldId]` is non-empty, run
  `AdfToMarkdown`/`HtmlToMarkdown` to seed the editor. Otherwise fall back
  to the stripped value.
- On modal-edit-save: run `MarkdownToAdf`/`MarkdownToHtml`, ship that as the
  field value. Update `fieldRichValues` cache with the new doc.
- If the user **didn't open the editor** for a field, we never re-emit it on
  any subsequent ticket update — fixes the silent-loss path.

### 3. Real-merge for offline replay

Today's offline flow: queue the new value, replay verbatim when online —
overwrites whatever the server has now.

v2 goal: when replaying, fetch current server doc, attempt a 3-way merge
between (original-at-edit-time, user's edit, server's current), and only
overwrite if the merge succeeds. On conflict, surface to the user.

**Storage change to the offline queue (`AppController_IssueCreateOffline.cpp`,
`PendingFieldEdit`):**

- Add `originalRichValueAtEdit` to the pending-edit record (the
  `fieldRichValues` snapshot from when the user opened the editor).
- Persist alongside the existing pending edit.

**Replay flow:**

1. Fetch fresh ticket from backend.
2. Compare `originalRichValueAtEdit` to `fetchedRichValue`:
   - **Identical** → no concurrent change; ship the user's edit. Done.
   - **Different** → 3-way merge attempt. Convert all three to Markdown,
     run a line-based merge (similar to git's recursive strategy), and:
     - **Clean merge** → ship merged Markdown converted to ADF/HTML.
     - **Conflict** → enter conflict-resolution UI (next item).

**3-way merge implementation:**

- Use a simple line-based diff/merge — port a small public-domain LCS-based
  3-way merge (~400 LoC). Good enough for paragraph-level changes.
- Block-level granularity (paragraph) reduces false conflicts vs.
  character-level.

### 4. Conflict-resolution UI ("merging tool")

When 3-way merge can't produce a clean result, the user needs a way to
resolve. Modal layout:

```
+---------------------------------------------------------------+
| Merge conflict — Description on ABC-123                       |
+---------------------------------------------------------------+
| Server's version (now)      | Your edit                       |
| [read-only Markdown]        | [editable Markdown]             |
|                             |                                 |
+---------------------------------------------------------------+
| Merged result (you decide)                                    |
| [editable Markdown — pre-filled with conflict markers]        |
+---------------------------------------------------------------+
| [Use server's]  [Use mine]  [Save merged]  [Cancel — keep queued] |
+---------------------------------------------------------------+
```

- Conflict markers in the merged area: `<<<<<<< server`, `=======`,
  `>>>>>>> yours`. User edits to clean them up; Save validates none remain.
- "Cancel — keep queued" leaves the offline edit pending; user can retry
  later. Useful if the server's version is itself stale (someone else
  resolving).

### 5. Expanded field coverage

v1 only routes Jira `description`/`environment` and Plane `description` to
the modal. v2 expands to **all** fields where `FieldUsesAdfDocument` returns
true — this picks up:

- Custom textarea fields (Jira `customLower.find("textarea")`).
- Wiki-renderer fields (`jira-wiki-renderer`).
- Doc-typed customfields (`field.Type == "doc"`).
- Any field with `adf` or `atlassian-document` in its schema custom string.

Also expand:

- **Plane non-description multiline fields.** Audit `PlaneClient.cpp` for
  any other long-text fields and treat the same way.
- **Comments.** Today comments are likely separate code paths. Apply the
  same modal + Markdown fidelity treatment.
- **Worklog descriptions.** `TicketFieldEditor.cpp:796` already uses
  `InputTextMultiline` inline — migrate to the modal for consistency, or
  leave as inline (worklogs are small).

### 6. New-issue draft form and bulk import

`SmatchetNewIssueDraftUi.cpp` and `SmatchetBulkTicketsUi.cpp` need the same
modal editor and Markdown round-trip when filling description-like fields
on new issues. Wire the same modal component (extracted to a reusable helper
in v1 so v2 only adds the call sites).

### 7. Open design questions for v2

- **Editing as Markdown vs WYSIWYG?** v1 punts to plain text; v2 commits to
  Markdown. WYSIWYG (rendered preview while editing) would require a custom
  ImGui markdown renderer — not in scope; revisit only if user feedback
  demands it.
- **Live preview pane?** Optional split-pane in the modal showing rendered
  Markdown as the user types. Cheap to add (use md4c's HTML-output module,
  render via ImGui-markdown or similar). Could ship in v2.1.
- **Mentions/smart-links.** ADF `mention` nodes (`@user`) and Atlassian smart
  links are not Markdown-representable. v2 keeps them in raw-mode; v3 could
  add a custom Markdown extension like `@[displayName](mention://accountId)`.
- **Image/attachment paste.** Out of scope for v2. v3 candidate.

---

## Suggested PR sequence for v2

1. **PR-A:** Vendor md4c, add `MarkdownConvert` module skeleton with
   md→ADF and md→HTML only. Unit tests against fixture documents. No UI
   change yet — verify converters are correct in isolation.
2. **PR-B:** Add `fieldRichValues` to `CachedTicket`; populate from
   `JiraIssueSearch` and `PlaneClient`; persist via `IssueTableSerializer`.
   No behavior change yet — just plumbing.
3. **PR-C:** Wire the v1 modal editor to use Markdown round-trip:
   `AdfToMarkdown`/`HtmlToMarkdown` on open, `MarkdownToAdf`/`MarkdownToHtml`
   on save. Add raw-mode fallback. Now formatting is preserved end-to-end.
4. **PR-D:** Expand field coverage (item 5). Touch only routing logic.
5. **PR-E:** Offline-replay 3-way merge (item 3). Backend change.
6. **PR-F:** Conflict-resolution UI (item 4). Frontend on top of PR-E.
7. **PR-G:** New-issue draft and bulk import surfaces (item 6).

PRs A–C are the load-bearing fidelity work. D–G are linear and can be
parallelized once A–C land.

---

## Files most likely to change in v2

- `Source_Core/src/TicketFieldEditor.cpp` — modal editor seeds Markdown,
  saves through converter.
- `Source_Core/src/MarkdownConvert.{h,cpp}` — new.
- `Source_Core/include/CachedTicket.h` (or wherever the struct lives) —
  `fieldRichValues` map.
- `Source_Core/src/JiraIssueSearch.cpp` — populate rich values on parse.
- `Source_Core/src/PlaneClient.cpp` — populate rich values on parse.
- `Source_Core/src/IssueTableSerializer.cpp` — persist rich values.
- `Source_Core/src/TrackerFieldPayload.cpp` — emit ADF/HTML from Markdown
  instead of single-paragraph wrapper.
- `Source_Core/src/AppController_IssueCreateOffline.cpp` — store
  `originalRichValueAtEdit`; replay performs merge.
- `Source_Core/src/SmatchetNewIssueDraftUi.cpp` — call modal helper.
- `Source_Core/src/SmatchetBulkTicketsUi.cpp` — call modal helper.
- `ThirdParty/md4c/` — new vendored library.
- `CMakeLists.txt` — link md4c.

---

## Things that should stay out of v2 scope

- Image/attachment paste in descriptions (v3).
- Custom Markdown extensions for mentions and smart links (v3).
- WYSIWYG rendered editor (v3+).
- Real-time collaborative editing (out forever).
- Backend choice beyond Jira/Plane (handle when adding the next backend).
