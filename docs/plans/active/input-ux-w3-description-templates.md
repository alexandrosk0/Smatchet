# Plan — Input UX Wave 3: templates, preview and mentions in descriptions

> **Slug**: `input-ux-w3-description-templates` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Source**: [`docs/audits/INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 5.3 Wave 3, bullet 4, finding **#55** (§ 7.5, Medium, effort L, `no-templates-preview-mentions`).
>
> **Baseline**: every `file:line` below was read at `origin/develop` `79baaa7a4` (2026-11-20). Lines drift; the symbols are the anchor.
>
> **Mandatory rules cross-link**: `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Finding #55: a new issue starts with an empty description, the user types Markdown blind and cannot see the rendering until after the issue exists, and `@name` is raw text with no autocomplete. The same holds for the comment box.

What exists today, per surface:

| Surface | Widget | Buffer | Notes |
|---|---|---|---|
| Inline new-issue row | raw `ImGui::InputTextMultiline("##input")`, `SmatchetNewIssueDraftUi.cpp:551` (`DrawDraftTextFieldInput`) | fixed 64 KiB, `EnsureDraftEditBuf` | static hint line at `:560`; row keys read in `HandleDraftRowKeys` before the cells draw |
| Quick Create | raw `InputTextMultiline("##quickcreate_desc")`, `SmatchetQuickCreateIssueUi.cpp:449` | fixed `quickCreateDescBuf` | AI prefill lock `quickCreateDescUserEdited`; keys read in `ReadQuickCreateKeys` before the widgets draw |
| Comments | `smatchet::ui::SubmitMultilineInput("##CommentsPostBox")`, `SmatchetCommentsModalUi.cpp:605` (`DrawCommentsPostInput`) | fixed 16 KiB `PostBuf`, dictation-registered | Ctrl+Enter posts (Wave 2, 4.4) |
| Long-text editor (existing issues) | `TicketFieldEditor_Modal.cpp` | — | **already has** Ctrl+P Edit / Split / Preview |

Other facts the design rests on:

- **Templates exist only for comments and work logs.** `struct CommentTemplate {Id, Title, Text}` (`ConfigManager.h:33`), lists `QuickCommentTemplates` / `AnnotateCommentTemplates` / `WorkLogCommentTemplates`, edited in Preferences by `DrawCommentTemplateSectionBody` (`SmatchetPreferencesUi_Templates.cpp`). No tracker template API is called anywhere.
- **The preview renderer is ready.** `MarkdownPreviewRender::BuildPlan` / `RenderPlan` / `HashContent` / `MakePlan` (`MarkdownPreviewRender.h:105-142`) let a caller cache a parsed plan and draw it each frame without re-parsing. The comments list already does this.
- **Mentions are lossy today.** `EmitInlineMention` (`AdfToMarkdown.cpp:207`) flattens a Jira ADF mention to plain `@Name`, and `MarkdownToAdf` never emits a mention node. Opening an existing Jira description that holds a mention in the long-text editor and saving it turns the mention into plain text, silently (the node is not reported as dropped, so no raw-mode banner shows).
- **User search is Jira-only.** `ITrackerCollaboration::SearchUsersByQuery` (`ITrackerCollaboration.h:68`) is implemented by `JiraClient` alone; `AppController::SearchUsersByQueryTyped` (`AppController_CatalogAndFieldEdit.cpp:1455`) is the typed, worker-only call.
- **A saved user roster already exists.** Each pane's `AvailableUsers` is filled by the catalog fetch and persisted in `lookup_cache` under kind `users`, key `all` (`LookupPayloadsPure.h`, `SaveAvailableUsersForOfflineAsync` / `SeedAvailableUsersFromStoreAsync`). No new table is needed for offline mention suggestions.
- **Wire formats.** Jira descriptions and comments both go through `MarkdownToAdf` (`BuildAdfScalar`, `AdfCommentBodyFromMarkdown` in `TrackerFieldPayloadPure.cpp`), including offline replay. Plane goes through `MarkdownToHtml`. Linear and GitHub send Markdown verbatim.

**After this lands:** a user can start a description from a saved template, flip the description or comment box to a rendered preview before sending, and type `@` to pick a person from a list that works offline; on Jira the pick becomes a real mention that survives a later edit.

Wave 2 context this plan builds on and must not duplicate: the shared field and submit keys (4.2 / 4.4, merged: `SmatchetInput.h`, `SmatchetSubmitKeys.h`), the dialog contract (4.1, `SmatchetDialogScope.h`), the one matcher (4.7, `TextMatchPure.h`), tolerant parsing (4.5, #2441, merged) and drafts and undo (4.3, plan `input-ux-w2-drafts-undo`, which arrives with PR #2439, **open**).

## Approach

**One shared composer helper, three features that live in the text.** A new `smatchet::ui::composer` module (`Ui/SmatchetComposerAssist`) is attached to the existing description and comment boxes; it does not replace them, and the fixed buffers stay (dictation still needs them). It draws a small bar above the box (Template picker, Preview toggle, the Markdown hint) and an inline people popup under the caret. Everything the user produces with it is ordinary text in the buffer, so drafts (#2439), the offline queue, copy and paste carry it with no new storage.

**A mention is a Markdown link with a pseudo-scheme:** `[@Display Name](mention:<id>)`. md4c already parses it as a link, and the tree has the same precedent for attachments (`attachment:<id>`). The converters are the only place that knows what it means: `MarkdownToAdf` turns it into an ADF `mention` node (covering Jira create, field edit, comment, worklog comment and offline replay in one edit), `MarkdownToHtml` writes plain `@Name`, and `AdfToMarkdown` gains an opt-in to write the token back so the long-text editor round-trips mentions instead of flattening them. Mentions are offered only where the backend says it supports them (`SupportsMentions()`, Jira only in v1); elsewhere the popup never opens and any pasted token is downgraded to `@Name` before it is sent.

**Templates are local, preview is cached.** Description templates are a new config list reusing `CommentTemplate` plus an optional project key, edited in Preferences beside the existing template sections; local config works offline by construction. Applying one never destroys typed text (replace when the box is empty or still holds the last applied template, otherwise append). The preview toggle swaps the input for `RenderPlan` over a plan rebuilt after a 100 ms quiet period, never per frame. The one non-obvious trade-off: the people popup is an inline list that leaves the caret in the box (as GitHub, Linear and Jira do), which means it must claim Up / Down / Enter / Tab / Esc from the text field with ImGui key ownership; if the S4 probe shows that is not reliable on the vendored ImGui, the fallback is a focused filter popup (audit § 3 "Picker or filter popup" row), with no other design change.

Out of the audit's phase 3, the Ctrl+B / Ctrl+I wrappers are **not** in this plan: `Ctrl+B` is the global `view.sidebar.primary` binding (`KeybindingsConfig.cpp:271`) and the dispatcher fires Ctrl chords while a text field is active (`SmatchetUI.cpp`, `typingSafe`). See § Out of scope.

### Terms

Used with one meaning throughout; the glossary writes are assigned to the slices that make them real (§ Implementation handoff).

- **Mention token** — the text `[@Display Name](mention:<id>)` inside Markdown. Backend-neutral. `<id>` is the tracker's opaque account id.
- **Description template** — a locally saved `CommentTemplate` in `TrackerConfig::DescriptionTemplates`. A **project-scoped template** is one whose `ProjectKey` is set; it is still local, not fetched from the tracker. _Avoid_ "project template": in Jira that names the blueprint a project is created from.
- **Composer** — a multiline box whose text is sent to the tracker as a description or a comment. Not the AI assistant's chat input.
- **Saved user roster** — a pane's `AvailableUsers`, live or seeded from `lookup_cache` (`users` / `all`); what the Ui leaf calls "the saved roster". Not the **Group roster** of `Tracker/CONTEXT.md` (group name → members, transient).
- **Mention node** — the ADF inline node Jira stores. Only the converters see it.

## Interface contracts

### Tracker role interface (needs this approved entry — `Source/Core/src/Tracker/AGENTS.md` § Before you edit)

- `ITrackerCollaboration.h`, after `FetchUsersByAccountIds`: `virtual bool SupportsMentions() const { return false; }`. True means: a **mention token** in a description or comment sent through this backend becomes a real mention, and `SearchUsersByQuery` returns ids valid for it. Precedent and rule: `ITrackerFieldCatalog::SupportsIssueTransitions()`; "an optional backend feature exposes a capability flag", default `false`.
- `Tracker/JiraClient.h`: `bool SupportsMentions() const override { return true; }`. No other client or fixture changes; `PlaneClient`, `LinearClient`, `GitHubClient` inherit `false`.
- No new virtual for the search itself: `SearchUsersByQuery(const TrackerConfig&, const std::string&)` is reused as is.

### App facade

- `Interfaces/IAppUsers.h`: `virtual bool FocusedBackendSupportsMentions() const { return false; }` (default-bodied so the one test fake, `tests/Commands/BuiltinFacetCommands.test.cpp`, needs no edit).
- `AppController` override, hop by hop: `std::atomic_load(&focusedContext().Backend)` → null-check → `backend->Collaboration()` → null-check → `->SupportsMentions()`. UI-thread safe (no I/O). The composer reads it once when it opens, not per frame.
- `AppController.h`, new public const method beside `SearchUsersByQueryTyped`:
  `Result<std::vector<TrackerUser>, TrackerError> SearchUsersForBackendTyped(const std::string& expectedCacheBackendKey, const std::string& query) const;`
  Worker-only. It latches `focusedContext()` **once**, compares that context's `CacheBackendKeyCopy()` with `expectedCacheBackendKey`, and on a mismatch returns `TrackerErrorInvalidRequest(...)` without sending a request; otherwise it calls `SearchUsersByQuery(ConfigManager::Load(), query)` on that same latched context's `Collaboration()`. Why not reuse `SearchUsersByQueryTyped`: that call resolves the focused pane on the worker, so a focus change between the keystroke and the send could answer a query with another site's account ids, and a wrong id in a mention notifies the wrong person.

### Converters (`Ui/MarkdownConvert.h` and its TUs)

- `std::string AdfToMarkdown(const nlohmann::json& adf, std::vector<std::string>* outDroppedNodeTypes = nullptr, bool emitMentionTokens = false);` — third parameter added, default keeps every current caller's output byte-identical (comment display, offline merge text, the offline-queue viewer, MCP / AI context).
- `MarkdownToAdf`: an `MD_SPAN_A` whose href is `mention:<valid id>` produces one inline node `{"type":"mention","attrs":{"id":"<id>","text":"@Display Name"}}` instead of a `link` mark. Marks nested inside the token are dropped. An invalid id leaves an ordinary text run (`@Display Name`), never a link to `mention:…`.
- `MarkdownToHtml`: a `mention:` link writes escaped plain `@Display Name`, never `<a href="mention:…">`.
- `MarkdownPreviewRender`: a run whose href starts with `mention:` draws in the theme's existing link colour without underline and is never passed to the link-open path. No new palette token.
- New pure header `Ui/MarkdownMentionPure.h` (namespace `smatchet::ui::mention`): `IsValidMentionId` (`[A-Za-z0-9:_-]{1,128}`, hand-written scan, no `std::regex`), `FormatMentionToken(displayName, id)` (backslash-escapes the Markdown-significant characters in the name — backslash, square brackets, `*`, `_` and the backtick — so the name can neither close the link text nor pick up emphasis; the ADF `text` attribute gets the unescaped name), `ParseMentionHref(href, outId)`, `DowngradeMentionTokens(markdown)` (every token → `@Display Name`).

### Shared UI contract

- `Ui/SmatchetSubmitKeys.h`: `SubmitMultilineInput` gains two trailing defaulted parameters, `ImGuiInputTextCallback callback = nullptr, void* callbackUserData = nullptr`. The implementation ORs the caller's callback flags into the ones it already builds and forwards both to `SmatchetLocalizedImGui::InputTextMultiline`, which already takes them. The existing call sites (`TicketFieldEditor_Worklog.cpp`, `AnnotateAnalysisUi_Window.cpp`) compile unchanged.
- New `Ui/SmatchetComposerAssist.h` (namespace `smatchet::ui::composer`), the only header hosts include:
  - `struct ComposerAssistState` — per-composer, owned by the host TU as a file-static (keeps `SmatchetUiSession.h` out of the write set).
  - `struct ComposerOptions { bool templates; bool preview; bool mentions; }` — which of the three a host turns on.
  - `void BeginFrame(ComposerAssistState&)` — called by the host **before** its own key reads; when the people popup was open last frame it claims Up / Down / Enter / KeypadEnter / Tab / Esc for this frame.
  - `bool PopupOwnsKeys(const ComposerAssistState&)` — hosts skip their own Esc and plain-Enter reads while it is true (Ctrl+Enter is still theirs).
  - `void DrawBar(AppController&, ComposerAssistState&, const ComposerOptions&, char* buf, std::size_t bufSize, const std::string& projectKey)` — Template picker, Preview toggle, hint.
  - `bool PreviewActive(const ComposerAssistState&)` and `void DrawPreview(ComposerAssistState&, const char* buf, const ImVec2& size)`.
  - `ImGuiInputTextCallback InputCallback()` with `void* CallbackUserData(ComposerAssistState&)` — reads caret and text, applies a pending splice inside the callback (bounded by `BufSize`), as `ApplyInlineReplace` does for the query box.
  - `void DrawMentionPopup(AppController&, ComposerAssistState&, const ImRect& fieldRect)`.
  - `std::string TextForSend(const ComposerAssistState&, const char* buf, const std::string& sendBackendKey)` — the buffer as is when mentions are supported and `sendBackendKey` equals the key latched at open; `DowngradeMentionTokens(buf)` otherwise. Every host calls it where it hands text to a create or a post.

### Config (additive; older builds ignore both keys)

- `CommentTemplate` gains `std::string ProjectKey;` — JSON `project_key`, written only when non-empty, read when present and a string.
- `TrackerConfig::DescriptionTemplates` — `std::vector<CommentTemplate>`, JSON key `description_templates`. Load in `LoadListFields` (`ConfigManager_Load.cpp`) with the same shape as `quick_comment_templates`: key present and an array → take it (an empty array stays empty); key absent → `GetDefaultDescriptionTemplates()` (two built-ins: "Bug report" — steps, expected, actual, environment; "Task" — goal, acceptance criteria). Save beside the existing template lists in `ConfigManager_Save.cpp`.
- Preferences schema: section `editing.description_templates` ("Description templates", `SaveSemantics::Autosave`) and its list descriptor, in `PreferencesSchema.cpp`, drawn from `DrawEditingPreferencesTab`.

### Unchanged — stated so nobody goes looking

- `ITrackerBackend.h` and the other five role interfaces; `IssueDraft`, `IssueCreatePipeline`.
- `RegisterCommand` entries, Lua bindings and `AppController_LuaStubs.cpp`, MCP tool schemas. `users.search` keeps its behaviour.
- SQLite: no schema change. `lookup_cache` kind `users`, key `all` is read through the pane's `AvailableUsers`; nothing new is persisted.
- The drafts file (`smatchet_drafts.json`, #2439): no new surface and no format change.
- Per-backend view storage; the dock layout; the theme palette.

## Files to modify

Grouped by slice (§ Implementation handoff). Existing symbols are in the probe table; new files and symbols follow it.

### File-level changes

| File | Symbol | CMake variable |
|---|---|---|
| `Source/Core/src/Ui/MarkdownToAdf.cpp` | `AdfEnterSpan` | `CORE_SOURCES` |
| `Source/Core/src/Ui/MarkdownToAdf.cpp` | `AdfLeaveSpan` | `CORE_SOURCES` |
| `Source/Core/src/Ui/MarkdownToHtml.cpp` | `MD_SPAN_A` | `CORE_SOURCES` |
| `Source/Core/src/Ui/AdfToMarkdown.cpp` | `EmitInlineMention` | `CORE_SOURCES` |
| `Source/Core/include/Ui/MarkdownConvert.h` | `AdfToMarkdown` | `CORE_SOURCES` |
| `Source/Core/src/Ui/MarkdownPreviewRender.cpp` | `RenderPlan` | `CORE_SOURCES` |
| `Source/Core/src/TicketFieldEditorLongTextPure.cpp` | `ComputeRoundTripPreview` | `CORE_SOURCES` |
| `Source/Core/include/ITrackerCollaboration.h` | `FetchUsersByAccountIds` | `CORE_SOURCES` |
| `Source/Core/include/Tracker/JiraClient.h` | `SupportsIssueTransitions` | `CORE_SOURCES` |
| `Source/Core/include/Interfaces/IAppUsers.h` | `IAppUsers` | `CORE_SOURCES` |
| `Source/Core/include/AppController.h` | `SearchUsersByQueryTyped` | `CORE_SOURCES` |
| `Source/Core/src/AppController_CatalogAndFieldEdit.cpp` | `SearchUsersByQueryTyped` | `CORE_SOURCES` |
| `Source/Core/include/Tracker/CollaborationPreconditionPure.h` | `CollaborationResultToResult` | `CORE_SOURCES` |
| `Source/Core/include/Config/ConfigManager.h` | `CommentTemplate` | `CORE_SOURCES` |
| `Source/Core/src/Config/ConfigManager_Views.cpp` | `CommentTemplate` | `CORE_SOURCES` |
| `Source/Core/src/Config/ConfigManager_Load.cpp` | `LoadListFields` | `CORE_SOURCES` |
| `Source/Core/src/Config/ConfigManager_Save.cpp` | `QuickCommentTemplates` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp` | `DrawCommentTemplateSectionBody` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp` | `DrawEditingPreferencesTab` | `CORE_SOURCES` |
| `Source/Core/src/Ui/PreferencesSchema.cpp` | `editing.quick_comments` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `DrawDraftTextFieldInput` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `HandleDraftRowKeys` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp` | `TrySubmitNewIssueDraft` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp` | `ReadQuickCreateKeys` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp` | `quickCreateDescUserEdited` | `CORE_SOURCES` |
| `Source/Core/include/Ui/SmatchetSubmitKeys.h` | `SubmitMultilineInput` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetSubmitKeys.cpp` | `SubmitMultilineInput` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `DrawCommentsPostInput` | `CORE_SOURCES` |
| `Source/Core/src/Ui/SmatchetCommentsModalUi.cpp` | `KickCommentPost` | `CORE_SOURCES` |
| `Source/Core/src/SmatchetLocalization.cpp` | `SmatchetLocalization` | `CORE_SOURCES` |

Per-file rationale:

1. `MarkdownToAdf.cpp` — token → ADF `mention` node (S1).
2. `MarkdownToHtml.cpp` — token → plain `@Name` (S1).
3. `AdfToMarkdown.cpp`, `MarkdownConvert.h` — `emitMentionTokens` opt-in in `EmitInlineMention` (S1).
4. `MarkdownPreviewRender.cpp` — a `mention:` run draws as a mention and never opens (S1).
5. `TicketFieldEditorLongTextPure.cpp` — the long-text seed and round-trip preview pass `emitMentionTokens = true`, which ends the lossy save (S1).
6. `ITrackerCollaboration.h`, `JiraClient.h` — `SupportsMentions` (S2).
7. `IAppUsers.h`, `AppController.h`, `AppController_CatalogAndFieldEdit.cpp`, `Tracker/CollaborationPreconditionPure.h` — `FocusedBackendSupportsMentions`, `SearchUsersForBackendTyped` and its pure refusal decision (S2).
8. `ConfigManager.h`, `ConfigManager_Views.cpp`, `ConfigManager_Load.cpp`, `ConfigManager_Save.cpp` — `ProjectKey`, `DescriptionTemplates`, defaults (S3).
9. `SmatchetPreferencesUi_Templates.cpp`, `PreferencesSchema.cpp` — the "Description templates" section, reusing `DrawCommentTemplateSectionBody` with a project-key input shown only for this list (S3).
10. `SmatchetNewIssueDraftUi.cpp`, `SmatchetQuickCreateIssueUi.cpp` — attach the composer: bar and preview (S3), then callback, popup and key hand-off (S4).
11. `SmatchetSubmitKeys.{h,cpp}` — callback pass-through (S5).
12. `SmatchetCommentsModalUi.cpp` — attach the composer to the post box; send `TextForSend` (S5).
13. `SmatchetLocalization.cpp` — new keys, English column (S3, S4, S5; each slice adds only its own rows).
14. `tests/CMakeLists.txt` — register each new `tests/Core/*.test.cpp` (configure FATALs on an unlisted test).
15. `docs/guides/keyboard-shortcuts.md` — Ctrl+P preview and the people-popup keys (S3, S4).
16. `Source/Core/src/Tracker/CONTEXT.md` — **Mention token** (S1). `Source/Core/src/Ui/AGENTS.md` § Before you edit — one bullet: a description or comment box gets templates, preview and mentions through `SmatchetComposerAssist`, never its own popup (S4).

### New files and symbols

`git ls-files` shows no existing file matching `compos*`, `mention*` or `*DescriptionTemplate*` under `Source/Core/{include,src}`. Core `.cpp` files are picked up by `file(GLOB_RECURSE CORE_SOURCES …)` for both `SmatchetStandalone` and `SmatchetCore_DX12`; no list edit.

| New file | Holds | Slice |
|---|---|---|
| `Source/Core/include/Ui/MarkdownMentionPure.h` | token format, parse, id check, downgrade (header-only, no ImGui, no md4c) | S1 |
| `Source/Core/include/Ui/DescriptionTemplatePure.h` | `TemplatesForProject`, `ApplyTemplate` (header-only) | S3 |
| `Source/Core/include/Ui/SmatchetComposerAssist.h`, `Source/Core/src/Ui/SmatchetComposerAssist.cpp` | the composer module (§ Interface contracts) | S3 (bar, preview), S4 (popup) |
| `Source/Core/include/Ui/ComposerMentionPure.h` | trigger scan, query bounds, splice offsets, key decisions, candidate ranking, debounce and stale-drop decisions (header-only) | S4 |
| `tests/Core/MarkdownMentionPure.test.cpp` | bucket A | S1 |
| `tests/Core/DescriptionTemplatePure.test.cpp` | bucket A | S3 |
| `tests/Core/ComposerMentionPure.test.cpp` | bucket A | S4 |
| `tests/ui/description_composer.test.cpp` (+ `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`, `scripts/dev/test-ui-description-composer.sh`) | bucket E | S3, extended in S4 and S5 |

New cases go into tests that already exist and are already registered: `tests/Core/MarkdownConvertAdf.test.cpp`, `tests/Core/MarkdownConvert.test.cpp`, `tests/Core/MarkdownPreviewPlan.test.cpp`, `tests/Core/TicketFieldEditorLongTextPure.test.cpp`, `tests/Core/ConfigManager.test.cpp`, `tests/Core/PreferencesSchema.test.cpp`, `tests/ui/prefs_schema_coverage.test.cpp`.

## Existing utilities reused

- `MarkdownPreviewRender::BuildPlan` / `RenderPlan` / `HashContent` / `MakePlan` — `Source/Core/include/Ui/MarkdownPreviewRender.h:105-142` — the cached-plan preview; the comments list is the usage model (`SmatchetCommentsModalUi.cpp:288`, `:396`).
- `CommentTemplate` and its `to_json` / `from_json` — `Source/Core/include/Config/ConfigManager.h:33` — the template record; no new struct.
- `DrawCommentTemplateSectionBody` — `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp` — the list-and-detail template editor in Preferences.
- `smatchet::textmatch` (`MakeQuery`, `Match`, `RankKey`, `RowListed`, `Scratch`) — `Source/Core/include/TextMatchPure.h` — ranking people and template names (Ui leaf rule: no hand-rolled `find`). `SmatchetMatchHighlight::DrawText` draws the matched letters.
- `FilterDropdownPure` (`MoveHighlight`, `DefaultHighlight`) — `Source/Core/include/FilterDropdownPure.h` — highlight movement in the popup.
- `smatchet::offline::KeyedLookupCache` + `RunKeyedFetch` — `Source/Core/include/KeyedLookupCache.h` — the mandated state machine for the keyed people search: no fetch while offline, backoff on failure, in-flight cleared on every path. `KeyedLookupCache::Freshness` + `DataFreshnessCue` (`Source/Core/include/DataFreshnessCue.h`) give the cue.
- `AppController::LaunchBackgroundTask` — `Source/Core/include/AppController.h` — the worker; joined in `~AppController`, so the composer owns no thread or future.
- `AppController::GetAvailableUsers` + `SeedAvailableUsersFromStoreAsync` — the saved user roster (cache-first candidates).
- `ApplyInlineReplace` — `Source/Core/src/Ui/SmatchetAutocompleteUi.cpp` — the in-callback splice pattern (`DeleteChars` / `InsertChars`), mirrored, not called (it is file-local to the query box).
- `TrackerQueryAcp_TickDebouncedUserSearch` — same file — the 220 ms debounce and request-id stale drop, mirrored as pure decisions in `ComposerMentionPure.h`.
- `SubmitMultilineInput`, `KeyHintButton` — `Source/Core/include/Ui/SmatchetSubmitKeys.h` — the comment box and the "Preview Ctrl+P" button.
- `SetKeyOwner` with `ImGuiInputFlags_LockThisFrame` — precedent `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp` — claiming keys from a focused text field.
- `SmatchetLocalization::T` / `Label` — every new string.
- `smatchet::drafts::Put` / `Get` (#2439, `Source/Core/include/DraftStore.h` once merged) — **not called by this plan**; it persists the comment buffer, tokens included, as it already does.
- `TicketFieldEditor_Modal.cpp` Ctrl+P cycle — the key and wording the new toggle matches.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — nothing is extracted or split. New logic goes into new files; the three host TUs gain call sites only. If `DrawDraftTextFieldInput` or `DrawCommentsPostInput` would pass the 200-line draw cap, the section-helper pattern applies (`docs/guides/imgui-draw-pattern.md`).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: closed composer costs one `BeginFrame` branch. Preview: `BuildPlan` runs only after 100 ms without an edit while preview is visible, keyed by `HashContent`; frames in between call `RenderPlan` on the cached plan. People popup: candidates are re-ranked only when the query or the candidate set changes, with one `textmatch::Scratch` per composer and the top 8 rows kept; per-frame work is drawing 8 rows.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: the people search runs only on `LaunchBackgroundTask`, annotated `/* PILLAR2_WORKER_ONLY */ // est-latency: 300ms`; the UI thread reads `KeyedLookupCache::Get`. No future is owned or waited on. Template apply and config reads use the in-memory `d.cfg`; saving goes through the existing debounced config save.
- **Pillar 3 (never crash)**: splices happen inside the ImGui callback and are bounded by `BufSize`; an over-long insert is refused and the popup says the text is full. Offsets are byte offsets validated against UTF-8 boundaries in the pure layer. The worker captures a `shared_ptr` to the cache, never the composer state. A malformed template entry is skipped on load, as today.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: every action has a key (Ctrl+P preview; Up / Down / Enter / Tab / Esc in the popup; the Template picker is a filterable, arrow-navigable combo). Mentions in the preview reuse the theme link colour, so no new contrast check. Sizes derive from font size and frame height, not pixels.
- **Pillar 6 (offline-first)**: templates are local config. People suggestions come from the saved user roster first and work with the tracker unreachable; no request is sent while `IsOfflineState`. The popup never shows a loading-only state when roster rows exist, and "No match" is shown only for a live answer — offline with an empty roster it says the people list is not saved yet. A failed search keeps the rows already shown and backs off (`KeyedLookupCache`).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

This plan-doc PR is pure docs: all five gates N/A for it. For the implementing slices:

1. **PR-fast CI** — S1 touches `MarkdownPreviewRender.cpp` → `long-text-open-large-adf` (and `ai-chat-history-render` where `SMATCHET_WITH_AI` is on) per `scripts/dev/perf-scenario-map.json`. S3 touches `SmatchetPreferencesUi*.cpp` → `preferences-slider-drag`. The composer TUs and the three hosts match no rule and run the `idle` fallback; S3 adds a map rule for `Source/Core/src/Ui/SmatchetComposerAssist.cpp` → `idle` so it is not reported as a gap.
2. **Pillar 2 static scanner** — **fires** in S4: one new network read (`SearchUsersForBackendTyped`), worker-only and annotated. Nothing new is reachable from an `ImGui::*` path.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` is not touched and the composer posts nothing to it (results are read from the keyed cache).
4. **Visible-cue bucket-E harness** — N/A: no new sync stall over 100 ms. The popup's cue rows are asserted in the S4 bucket-E test instead.
5. **Marker inventory** — N/A: no permanent `SMATCHET_UI_PERF_SCOPE` marker is added. Development measurements use `perf_temp:` markers, stripped before each PR.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against `long-text-open-large-adf` (S1) and `idle` (S3, S4, S5).

**Override**: `perf-out-of-band` PR label per `AGENTS.md` § Merge gates — not expected.

## Risks / non-goals

Risks:

- **Key ownership on the vendored ImGui (unverified).** The inline popup assumes `SetKeyOwner(..., LockThisFrame)` set before the text field draws stops both the field and the hosts' owner-less `IsKeyPressed` reads from seeing Up / Down / Enter / Tab / Esc, and that the NewFrame nav-cancel does not take the caret out of the multiline field on the Esc frame. Mitigation: S4 starts with a bucket-E probe (§ Open questions Q5); hosts also skip their own reads through `PopupOwnsKeys`; fallback is the focused filter popup.
- **Esc in a raw `InputTextMultiline` reverts typed text.** Any Esc that reaches the field while the popup is open could drop the description. Mitigation: Esc is claimed before the field draws; the bucket-E test asserts the text is intact after Esc closes the popup.
- **Wrong person mentioned.** An id from another site would notify a stranger. Mitigation: the composer latches the pane's cache backend key at open, `SearchUsersForBackendTyped` refuses a mismatched pane, roster rows come from that pane only, and results are keyed by backend key plus query.
- **Token leaks to a backend that cannot convert it.** Linear and GitHub send Markdown verbatim. Mitigation: the popup is gated on `SupportsMentions()`, and every composer send goes through `TextForSend`, which downgrades pasted tokens. Text sent by MCP or Lua callers is theirs and is not rewritten.
- **Long-text editor shows raw tokens.** After S1, an existing Jira description with a mention shows `[@Name](mention:id)` in Edit mode (and a mention in Preview). This is the price of a lossless save; the alternative is today's silent loss.
- **Stale saved user roster.** Offline rows can include people who left. Jira renders an unknown id as plain text; accepted.
- **Search is site-wide.** `SearchUsersByQuery` returns people on the Jira site, not only those who can see the issue's project; Jira does not notify a mentioned user who lacks access. Accepted for v1.
- **Ctrl+P rebinding.** No default binding uses Ctrl+P (the palette is Ctrl+Shift+P), but a user binding on Ctrl+P would fire alongside the toggle, exactly as it does in the long-text editor today. Accepted; the button is the other way in.
- **Save-format breaks.** Config: additive keys only, older builds ignore them, an older config loads with the two default templates. SQLite: none. Drafts file: none.
- **Backend leakage.** The only interface change is a `Supports*` flag. The ADF node is built in the converter that is already Atlassian-specific; the token is neutral.
- **Unreal vs standalone.** New TUs use ImGui and `imgui_internal.h` only (no GLFW, no Win32) and compile into `SmatchetCore_DX12` through the glob. Dictation keeps working because the fixed buffers stay. Each slice builds both targets. Bucket E runs on the standalone target only, so the Unreal quick-create path is covered by the compile gate and by sharing every line of composer code, not by its own UI test.
- **Localization key churn.** About 15 new keys under `composer.*` and `prefs.editing.description_templates.*`; `SmatchetLocalization.cpp` is also edited by the sibling Wave 3 slices, so each slice adds its rows in one block to keep merges trivial.
- **MCP wire format.** Unchanged: `AdfToMarkdown` defaults to today's output.
- **`std::regex` budgets.** None: the id check and the trigger scan are hand-written.
- **File collisions with open work.** `SmatchetCommentsModalUi.cpp`, `TicketFieldEditor_Modal.cpp`, `docs/guides/keyboard-shortcuts.md` ← #2439. `AppController.h`, `AppController_CatalogAndFieldEdit.cpp` ← #2450, #2452. `SmatchetQuickCreateIssueUi.cpp` ← #2450. `tests/CMakeLists.txt` ← #2452. `SmatchetNewIssueDraftUi.cpp`, `SmatchetCommentsModalUi.cpp` ← the attachments-by-drop-and-paste Wave 3 plan (Ctrl+V on the same boxes). `SmatchetLocalization.cpp`, `tests/CMakeLists.txt` ← the grid-spreadsheet plan (`input-ux-w3-grid-spreadsheet`, which arrives with PR #2461). Mitigation: each slice claims a plan-lock on its write set before editing, lands behind the PRs named in its Order line, and never uses a lock override.

Non-goals:

- No WYSIWYG editing (the audit rules it out for ImGui).
- No replacement of the fixed buffers with `SmatchetInput::Multiline` (blocked on the dictation router's string target, debt `2026-11-16-dictation-router-string-target`).
- No change to `SmatchetAutocompleteUi.cpp` — it belongs to the filter-without-JQL plan's surface.
- No grid, attachment, or query-box behaviour.

## Implementation handoff

Six slices, one PR each. S1 and S2 are independent and can run in parallel; S3 is independent of both.

### S1 — Mention token and converters

- **Owner subsystem**: Ui (`Source/Core/src/Ui/AGENTS.md`; `TicketFieldEditor*.cpp` is governed by the same leaf per `CONTEXT-MAP.md`).
- **Write set**: `Ui/MarkdownMentionPure.h` (new), `MarkdownToAdf.cpp`, `MarkdownToHtml.cpp`, `AdfToMarkdown.cpp`, `MarkdownConvert.h`, `MarkdownPreviewRender.cpp`, `TicketFieldEditorLongTextPure.cpp`, `Source/Core/src/Tracker/CONTEXT.md` (the **Mention token** term, and the stale **ITrackerCollaboration** line the stress-test found: it says `PlaneClient` returns `nullptr`, but `PlaneClient::Collaboration()` returns `this`), the five `tests/Core` files named above, `tests/CMakeLists.txt`.
- **Pre-resolved decisions**: `emitMentionTokens` defaults to `false` and only the two long-text call sites pass `true`. A mention inside a link, code span or fence is not converted. `MarkdownMentionPure.h` includes no ImGui, md4c or json header (grep-guard it in the test).
- **Inline inventory**: `MarkdownToAdf.cpp` `case MD_SPAN_A` in `AdfEnterSpan` (pushes a `link` mark) and in `AdfLeaveSpan`; `AdfToMarkdown.cpp` `EmitInlineMention`; `MarkdownToHtml.cpp` both `MD_SPAN_A` cases; `TicketFieldEditorLongTextPure.cpp` the two `AdfToMarkdown(` calls; other `AdfToMarkdown(` callers that must stay on the default: `JiraCommentMappingPure.cpp`, `OfflineQueueService.cpp`, `SmatchetOfflineQueueUi.cpp`.
- **Order**: none. Ships a user-visible fix on its own (mentions survive a long-text save).

### S2 — Capability flag and pane-bound search

- **Owner subsystem**: Tracker (`Source/Core/src/Tracker/AGENTS.md`) for the role interface; the facade edits ride along because the flag is unreachable from UI code without them and the whole slice is about 40 lines.
- **Write set**: `ITrackerCollaboration.h`, `Tracker/JiraClient.h`, `Interfaces/IAppUsers.h`, `AppController.h`, `AppController_CatalogAndFieldEdit.cpp`, `Tracker/CollaborationPreconditionPure.h` (the refusal decision as a pure function, beside `CollaborationResultToResult`) and `tests/Core/CollaborationPreconditionPure.test.cpp` (already registered). `AppController` is not linkable into the unit rig, so its method stays a thin wrapper over that function.
- **Pre-resolved decisions**: default `false`; Jira only. Plane stays `false` until Q3 is answered. The mismatch returns `InvalidRequest` (not retryable), so `KeyedLookupCache` does not treat it as a transport outage.
- **Order**: behind #2450 and #2452 (both edit `AppController.h` and `AppController_CatalogAndFieldEdit.cpp`), or rebased over whichever is still open.

### S3 — Description templates and preview on the create surfaces

- **Owner subsystem**: Ui. The four `Config/` files have no leaf (root rules apply) and ship in the same PR because a config list with no editor and no consumer is dead code.
- **Write set**: the four config files, `SmatchetPreferencesUi_Templates.cpp`, `PreferencesSchema.cpp`, `Ui/DescriptionTemplatePure.h` (new), `Ui/SmatchetComposerAssist.{h,cpp}` (new; bar and preview only), `SmatchetNewIssueDraftUi.cpp`, `SmatchetQuickCreateIssueUi.cpp`, `SmatchetLocalization.cpp`, `scripts/dev/perf-scenario-map.json`, `docs/guides/keyboard-shortcuts.md`, tests.
- **Pre-resolved decisions**:
  - Template filter: `ProjectKey` empty → every project; otherwise a case-insensitive match on the draft's project key. Project-scoped templates sort first.
  - The inline row has a description box only when the view shows a description column (existing behaviour); the bar follows the box and adds one frame height to that cell. Quick Create always has both.
  - The last applied template's text is kept in `ComposerAssistState` (session-only) for the replace-or-append decision.
  - Apply rule (`ApplyTemplate`, pure): buffer empty or equal to the last applied template's text → replace; otherwise append after one blank line. Text that would not fit the buffer is refused with a toast; nothing is truncated.
  - Apply happens only while the field is not active; the host writes the buffer and `FieldValues["description"]`, and drops ImGui's saved deactivated-field copy for that id (as `SmatchetInput.cpp` does) so the old text is not written back.
  - Quick Create: applying a template sets `quickCreateDescUserEdited = true` so AI prefill does not overwrite it.
  - No placeholders in template text (Q2).
  - Preview: a `KeyHintButton`, and Ctrl+P while the input holds the caret or the preview child window is focused (turning preview on focuses that child, so the same key turns it off). Session-only, off by default, same height as the input. Draft text is rendered as typed (no ADF or HTML round trip). Leaving preview returns the caret to the input. The host's Esc and Ctrl+Enter keep their meaning in preview.
  - The static hint line at `SmatchetNewIssueDraftUi.cpp:560` moves into the bar.
  - Comments keep their existing right-click quick templates; no Template picker there.
- **Order**: none on #2439. Behind #2450 for `SmatchetQuickCreateIssueUi.cpp`.

### S4 — People popup on the create surfaces

- **Owner subsystem**: Ui.
- **Write set**: `Ui/ComposerMentionPure.h` (new), `Ui/SmatchetComposerAssist.{h,cpp}`, `SmatchetNewIssueDraftUi.cpp`, `SmatchetQuickCreateIssueUi.cpp`, `SmatchetLocalization.cpp`, `docs/guides/keyboard-shortcuts.md`, `Source/Core/src/Ui/AGENTS.md`, tests.
- **Pre-resolved decisions**:
  - **Trigger**: `@` at the start of the text or after whitespace or one of `(["'`; not inside an inline code span or a fenced block; not `x@y`. Query = bytes from `@` to the caret, at most 64 bytes, at most one inner space, no newline.
  - **Closes when**: the caret leaves the query, a second space or a newline is typed, a row is accepted, Esc, or the field loses the keyboard.
  - **Candidates**: the saved user roster of the latched pane ranked with `textmatch` (exact-first, `RowListed`), then, when online and the query has 2+ bytes, a search debounced 220 ms through `KeyedLookupCache` keyed `<backendKey>\n<folded query>`; results merge by `AccountId`. The cache is session-only and cleared when the backend key changes or it passes 64 keys. Accounts whose `AccountType` is `app` or `customer` are left out, as the query box does.
  - **Rows**: `DisplayName` only (no email address on screen). Cue rows per Pillar 6 above.
  - **Keys** (audit § 3 "Picker or filter popup" row, adapted to an inline caret): Up / Down move; Enter or Tab accept the highlighted row; with no rows they are not claimed (Enter types a new line, Tab moves on); Esc closes the popup only — the caret stays and the host's Esc does not fire; Ctrl+Enter is never claimed, so submit still works; a click accepts.
  - **Accept**: replace `@query` with `FormatMentionToken(displayName, accountId)` plus one space, caret after it.
  - **Gate**: `FocusedBackendSupportsMentions()` read once when the composer opens. False → no popup, and `TextForSend` downgrades tokens.
  - **Send**: both create hosts pass the description through `TextForSend` where they submit the draft (`TrySubmitNewIssueDraft`, the Quick Create submit), with `app.FocusedCacheBackendKey()` as the send key.
  - **One search at a time**: at most one search is in flight per composer; a newer query starts when it finishes. Connectivity for `TryBeginFetch` comes from `app.GetLastTrackerConnectivityState()`.
  - **Pane binding**: backend key latched with `app.FocusedCacheBackendKey()` when the composer opens; never re-read on the worker. `GetAvailableUsers()` answers for the focused pane, so while the popup is open each frame compares `FocusedCacheBackendKey()` with the latched key and closes the popup on a mismatch instead of listing another site's people.
- **Order**: after S1, S2, S3. First task is the Q5 probe.

### S5 — Comments: people popup and preview

- **Owner subsystem**: Ui.
- **Write set**: `SmatchetSubmitKeys.{h,cpp}`, `SmatchetCommentsModalUi.cpp`, `SmatchetLocalization.cpp`, tests.
- **Pre-resolved decisions**: the composer state is a file-static beside `s_CommentsState` and is reset where that is. The splice runs inside `SubmitMultilineInput`'s callback, so #2439's by-value draft compare sees the new text the same frame; no `drafts::` call is added. `KickCommentPost` latches its target when it posts (`app.LatchPendingActionTarget()`), so it sends `TextForSend(state, buf, target.BackendKey)`: a token never goes to a different site than the one whose people were listed.
- **Order**: **after #2439 merges** — both edit `DrawCommentsPostInput`, and the splice must sit before its draft compare. After S4.

### S6 — People popup in the long-text editor (only if Q1 is yes)

- **Owner subsystem**: Ui. **Write set**: `TicketFieldEditor_Modal.cpp`, tests. **Order**: after #2439 and S4.

Post-ship (each slice's PR, never a direct push): append § Implementation log, § Deviations and § Verification (actual); the last slice flips Status and does the archive move.

## Verification

Zero manual steps.

- **Bucket A (pure-logic doctest, `test-authoring` skill § Unit rig)**:
  - `MarkdownMentionPure`: name escaping (`]`, `\`), id validation (empty, 129 bytes, a space, a `)`), href parse, downgrade of one / several / malformed tokens.
  - `MarkdownConvertAdf`: token → `mention` node with `id` and `text`; invalid id → text; nested marks dropped; token inside a code span untouched; `AdfToMarkdown` default output unchanged for a mention; opt-in output is a token; token → ADF → token round trip is stable.
  - `MarkdownConvert`: HTML output is `@Name` with no `href="mention:`.
  - `MarkdownPreviewPlan`: a mention run keeps its href and is flagged non-opening.
  - `TicketFieldEditorLongTextPure`: seed and round-trip preview keep a mention.
  - `ComposerMentionPure`: trigger at start, after space, after `(`; no trigger in `a@b`, in a code span, in a fence; query bounds (64 bytes, second space, newline); splice offsets on multi-byte UTF-8; key decisions with rows and with none; debounce and stale-id drop.
  - `DescriptionTemplatePure`: project filter and order; replace on empty; replace when the buffer equals the last template; append otherwise; refusal when it would not fit.
  - `ConfigManager`: `description_templates` and `project_key` round trip; absent key → defaults; empty array stays empty; a pre-change config loads.
  - `PreferencesSchema`: the new section and descriptor are well-formed.
  - `CollaborationPreconditionPure` (S2): a mismatched backend key is refused as `InvalidRequest`; a matching key passes.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)** — `tests/ui/description_composer.test.cpp` on the deterministic Jira backend (pattern: `tests/ui/jira_deterministic_backend.test.cpp`) with a seeded roster:
  - Template picker fills an empty Quick Create description; picking a second template appends when text was typed.
  - Ctrl+P shows the rendered preview and Ctrl+P again returns the caret to the input with the text intact.
  - Typing `@jo` opens the popup with roster rows; Down then Enter inserts the token; the description buffer holds `[@…](mention:…)`.
  - Esc closes the popup, leaves Quick Create open and the text intact; in the inline row it does not start the discard flow.
  - Enter with no rows types a new line; Ctrl+Enter with the popup open still submits.
  - Offline: the popup lists saved people with the offline cue and sends no request (asserted on the deterministic backend's `SearchUsersByQuery` call count; add the counter if it has none); with an empty roster it does not say "No match".
  - Comments (S5): a mention typed in the post box survives closing and reopening the window (#2439's store), and the posted body carries the token.
  - `tests/ui/prefs_schema_coverage.test.cpp` covers the new section.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no new CLI command or scenario; text assertions in bucket E cover what a screenshot would. No new threading primitive (`KeyedLookupCache` + `LaunchBackgroundTask`), so the existing sanitizer lanes are the coverage.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) for every slice.
- **Pillar 6 scan**: the offline heuristics (`offline-loading-only-render`, `offline-network-read-ungated`, `offline-inflight-latch-unguarded`) stay clean on the changed `.cpp` files.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script). A red doc-validation job blocks merge even though non-required. Also `bash agent-layer/agents/scripts/core/plan-doc-table-probe.sh` on this file: every row resolves.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against this plan before it is final; outcome recorded in § Stress-test record below.
- **Manual residue**: none. If the Q5 probe forces the fallback popup, the bucket-E cases above change wording, not count.

### Stress-test record (`grill-with-docs`)

Run 2026-11-20 against `Source/Core/src/Tracker/CONTEXT.md`, `docs/CONTEXT.md`, the five leaf `AGENTS.md` files, ADR-0026 and the code at `79baaa7a4`. It ran without the user in the loop: every question the code or the docs could answer was answered there, and the rest are in § Open questions.

| # | Question | Answer | Effect on the plan |
|---|---|---|---|
| 1 | "Project template": the tracker's or ours? | Ours; and in Jira the phrase already means a project-creation blueprint. | Term renamed **project-scoped template**; tracker-sourced templates stay out of scope. |
| 2 | "Roster": does it collide with the glossary's **Group roster**? | In name, not in meaning. | Term sharpened to **saved user roster**, with the distinction written down. |
| 3 | Storage: is there really a persisted user list? | Yes: `lookup_cache` kind `users`, key `all` (`LookupPayloadsPure.h:20-22`), written by `SaveAvailableUsersForOfflineAsync`, read by `SeedAvailableUsersFromStoreAsync`. | No schema change; no new table claimed. |
| 4 | Storage: where do templates live, and what does an old config do? | Config JSON, loaded in `LoadListFields` with the `quick_comment_templates` shape (`ConfigManager_Load.cpp:664-674`): absent key → defaults. | Load rule written to match; an empty array stays empty. |
| 5 | Storage: does the drafts store need a new surface for tokens? | No. #2439's `drafts::Put` stores the buffer text, and a token is text. | No `drafts::` call in this plan; S5 is ordered behind #2439 only for the shared lines. |
| 6 | The roster call answers for the focused pane. What if focus moves while the popup is open? | It would list another site's people. | A per-frame key compare closes the popup; the search is pane-bound by `SearchUsersForBackendTyped`. |
| 7 | The Ui leaf says keyed lookups go through `KeyedLookupCache`. Does the people search? | The first design polled a future, as the query box does. | Switched to `KeyedLookupCache` + `RunKeyedFetch` on `LaunchBackgroundTask`; the composer owns no future, so it has no shutdown-join contract. |
| 8 | The Ui leaf says take the typed call so an unreachable tracker is not read as "none". | `IAppUsers::SearchUsersByQuery` flattens the error to a string. | The new call returns `TrackerError`; "No match" only for a live answer. |
| 9 | What does a token do on a backend that cannot convert it? | Plane: the HTML converter would emit a dead link. Linear and GitHub: posted literally. | `MarkdownToHtml` writes `@Name`; `TextForSend` downgrades at every composer send. |
| 10 | After the app creates a mention, does editing that description keep it? | No: `EmitInlineMention` flattens it. | The `emitMentionTokens` opt-in for the long-text editor is part of S1, not optional. |
| 11 | Can a display name break the token? | Yes, with square brackets or emphasis characters. | Escaping rule widened and tested. |
| 12 | Does Esc with the popup open lose text? | A raw `InputTextMultiline` reverts on Esc if the key reaches it. | Named as a risk with a bucket-E assertion; the Q5 probe decides inline versus focused popup. |
| 13 | Does the glossary agree with the code on who has **ITrackerCollaboration**? | No: the glossary says `PlaneClient` returns `nullptr`; `PlaneClient.cpp:179` returns `this`. | Stale line; the fix is assigned to S1, which already edits that file. Not fixed here, to keep this PR to one file. |
| 14 | Does the token format need an ADR? | No. It is a real trade-off and would surprise a reader, but it is cheap to reverse: tokens persist only in local drafts and queued writes, and Jira stores nodes. | The **Mention token** glossary term (S1) carries the decision. |
| 15 | Is the inline row always a description surface? | Only when the view has a description column. | Stated in S3. |
| 16 | Does the Unreal quick-create path get UI-test coverage? | No; bucket E is standalone. | Stated in Risks. |

## Open questions

1. **Q1 — user**: should the people popup also be offered in the long-text editor for existing issues (S6)? Default: no in v1. S1 already makes mentions survive an edit there, and they can be typed in by hand as tokens.
2. **Q2 — user**: placeholders in description templates (`{project}`, `{type}`, `{me}`, `{date}`)? Default: none in v1.
3. **Q3 — user, then a probe**: Plane mentions. Default: off in v1. Turning them on needs someone to confirm against a live Plane API that `<mention-component …>` survives its sanitizer, then S2's flag for `PlaneClient`, an HTML converter branch, and `HtmlSubsetToMarkdown` learning the tag.
4. **Q4 — user**: template scope. Default: optional project key only. Two Jira sites can share a project key, and templates are not scoped by site or issue type. Say if either is wanted.
5. **Q5 — a probe at the start of S4**: on the vendored ImGui, does `SetKeyOwner(key, id, ImGuiInputFlags_LockThisFrame)` before the field draws keep Enter / Tab / Up / Down / Esc from both `InputTextMultiline` and owner-less `IsKeyPressed`, and does the caret stay in the field on the Esc frame? Answered by the first bucket-E case; "no" selects the focused filter popup.
6. **Q6 — user**: confirm the two built-in templates ("Bug report", "Task") are wanted, or ship the list empty.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them. Sweep result at authoring time: no hit for `mention:`, `SmatchetComposerAssist`, `DescriptionTemplates` or `description_templates` in any of them.

- **Ctrl+B / Ctrl+I formatting wrappers** (audit #55 phase 3). Needs a dispatcher rule that a focused text field owns its Ctrl chords, which changes keybinding behaviour app-wide. Follow-up plan; file a `docs/self-improvement/categories/` entry when S4 ships.
- **Tracker-sourced templates** (Linear `templates`, GitHub `.github/ISSUE_TEMPLATE`). Would need a new role-interface read plus a `lookup_cache` kind. Follow-up plan if wanted.
- **Mentions on Plane, Linear, GitHub.** Q3 covers Plane; Linear and GitHub have no roster or user search in the tree. No action.
- **Drafts for the inline row and Quick Create** (#51's new-issue half). Owned by the drafts follow-up; when it lands it stores tokens and template text as plain text with no change here.
- **Template picker in the comment box.** Comments keep the right-click quick templates; sharing `BuildTemplateCommentBody` means editing a grid file. No action.
- **Attachments by drop and paste (#54, #137), the grid (#26–#30), the search box (#61, #63, #68).** Sibling Wave 3 plans.
- **Converging the two debounced user searches** (query box and composer). Recorded as duplication debt when S4 ships; not designed here because `SmatchetAutocompleteUi.cpp` is on a sibling plan's surface.
- **`ShellExecuteA` on any href from other people's comments** (`MarkdownPreviewRender.cpp`). Pre-existing; this plan only guarantees `mention:` never reaches it.
- **Mentions lost in the offline merge text** (`OfflineQueueService.cpp` flattens them when it builds merge text). Pre-existing; no action here.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
