<!-- index-summary: input UX Wave 2 piece 4.5 — one parse-and-explain layer (durations, dates, issue URLs, site URLs, queries) with a shared inline error under the field, plus a toast on every remaining silent failure -->
# Plan — Input UX Wave 2: tolerant parsing with inline errors

> **Slug**: `input-ux-w2-tolerant-parsing` (matches this file's basename without `.md`).
>
> **Status**: `active`

## Context

[`INPUT_UX_AUDIT.md`](../../audits/INPUT_UX_AUDIT.md) § 4.5 names one shared fix for fourteen findings: dates accept one format, durations reject `1.5h`, a bad query or bad value fails later and elsewhere, and several failures reach only the log. Wave 1 (PR #2433, the base branch of this work) already closed three of the fourteen:

- #98: dictation failures toast.
- #117: toolbar Args JSON is checked as typed through `json_safe::ParseBounded`.
- #136: a rejected file drop toasts.

This plan closes what is still open: #38, #40, #63, #66, #80, #82, #115, #127 and #128. #53 and #58 are deferred; see § Out of scope.

**Intended outcome**: after this lands, a duration, date, issue reference, tracker site URL, executable path, chat URL or query either becomes a value or yields a message that names the bad part and says how to fix it. The message is drawn under the field while the user types. Every path that used to fail silently now raises a toast with its cause and its fix.

## Approach

**One pure parse-and-explain layer** in the Tracker strict zone, next to `TrackerFieldValueParser` (field-value parsing lives there; `Source/Core/src/Tracker/AGENTS.md`).

- Each parser returns a result struct holding either a value plus its canonical text, or a `ParseMessage`.
- A `ParseMessage` is an id from a closed enum, an optional argument (the offending token), and the byte span of that token.
- The parsers allocate only small strings and do no I/O and no ImGui calls.
- One text function, `DescribeParseMessage`, turns a message into user text. It holds a literal `SmatchetLocalization::Format("parse.…", "…%s…", arg)` call per id, so every string keeps its call-site English and a French row in `kEntries[]`, the same as any other localized call.

**One shared inline error widget** in `Ui/SmatchetInlineError.{h,cpp}`, used right after the field it describes:

- `DrawInlineParseError` draws one wrapped line under a form field in the theme's `ErrorText` colour.
- `DrawInlineParseHint` draws a dim preview of the parsed value, such as `= 1h 30m` or `= 2026-11-20 (Fri)`, once the text parses and differs from the canonical form.
- `DrawInlineParseErrorBelowItem` draws the same message on the foreground draw list just under the last item. It is for a grid cell, where an extra line would grow the row. It is not a window, so it cannot steal focus or collide with a hover tooltip.
- Under `SMATCHET_BUILD_UI_TESTS`, the widget records the last message drawn each frame. Bucket-E tests assert on that record.

**Editors commit the canonical form.** A tolerant parse that succeeds rewrites the buffer to the strict form downstream code already understands: `FormatWorkDurationFromSeconds`, `FormatJiraDateOrDateTimeForApi`, the bare issue key, or the normalised host. So `ParseWorkDurationToSeconds`, the sort path, the payload builders and the backends see exactly what they see today.

- That keeps this plan disjoint from [`duration-parser-unification`](../duration-parser-unification.md), which still owns the tracker-value parser and the grid sort path.
- A parse that fails never commits. Enter keeps the caret in the field with the message under it. A focus-loss in a grid cell ends the edit without a write and raises a toast with the message, because the cell cannot keep the caret.

## Interface contracts

There are no changes to `ITrackerBackend` or its role interfaces, to `RegisterCommand` entries, Lua bindings, MCP schemas, view storage, config or the SQLite format. The new shared contracts are:

- **`Source/Core/include/Tracker/TrackerFieldValueParseExplain.h`** (new; namespace `TrackerParseExplain`):
  - `enum class ParseMessageId` and `struct ParseMessage { ParseMessageId Id; std::string Arg; std::size_t At; std::size_t Len; }`.
  - `std::string DescribeParseMessage(const ParseMessage&)`.
  - `enum class BareNumberUnit { Minutes, Hours }` and `struct DurationParse { bool Ok; bool Empty; long long Seconds; std::string Canonical; ParseMessage Message; }`. `DurationParse ParseDurationInput(const std::string&, BareNumberUnit)`.
  - `enum class DateOrder { MonthFirst, DayFirst }` and `struct CivilDate { int Year, Month, Day; }`. `struct DateParse { bool Ok; bool Empty; CivilDate Date; ParseMessage Message; }`. `DateParse ParseDateInput(const std::string&, const CivilDate& today, DateOrder)`.
  - `int WeekdayOf(const CivilDate&)`, where 0 is Sunday.
  - `struct IssueReferenceParse { bool Ok; std::string Key; bool FromUrl; ParseMessage Message; }`. `IssueReferenceParse ParseIssueReference(const std::string&)`.
- **`Source/Core/include/Tracker/TrackerInputNormalisePure.h`** (new; namespace `TrackerInputNormalise`):
  - `enum class SiteField { JiraDomain, PlaneUrl, GitHubBaseUrl, GitHubOwner }` and `struct SiteUrlParse { bool Ok; bool Rewritten; std::string Value; std::string Owner, Repo, WorkspaceSlug; ParseMessage Message; }`. `SiteUrlParse NormaliseSiteField(const std::string&, SiteField)`.
  - `struct PathParse { bool Ok; bool Rewritten; std::string Value; ParseMessage Message; }`. `PathParse NormaliseExecutablePath(const std::string&)`.
  - `PathParse ValidateHttpUrl(const std::string&, bool allowEmpty)`.
- **`Source/Core/include/Tracker/JqlQueryValidatePure.h`** (new; namespace `JqlQueryValidate`): `ParseMessage ValidateQuery(const std::string& text, QueryDialect dialect, const std::vector<TrackerField>* fields)`. It returns `ParseMessageId::None` when the query is fine. It checks paren and quote balance, a dangling operator or keyword, and, when a catalog is given, a field name that is not in it.
- **`Source/Core/include/Ui/SmatchetInlineError.h`** (new): the three draw helpers above, plus a test seam `InlineErrorLastFrameForTests()`, which is `SMATCHET_BUILD_UI_TESTS` only.
- **`Source/Core/include/Ui/P4vLaunch.h`** (changed): `LaunchP4VcLike` gains an optional out-parameter, `std::string* outFailureReason = nullptr`. The default keeps every call site source-compatible.
- **`GridSearchInputClassifier.h`** (changed): `LooksLikeStructuredQuery` no longer treats a lone `(`, `)` or `!` as a query. It needs a comparison operator, a JQL keyword operator or a `field:value` token. Two additions:
  - A pasted issue URL classifies as `TicketKey`.
  - `std::string TicketKeyFromSearchInput(const std::string&, GridSearchBackend)` returns the key to jump to.
- **`TrackerFieldPayloadPure::ExtractIssueKey`** (behaviour widened): a pasted issue URL yields its key. A bare key behaves as today.

## Files to modify

**New — the parse-and-explain layer (Tracker, strict zone):**

1. `Source/Core/include/Tracker/TrackerFieldValueParseExplain.h` + `Source/Core/src/Tracker/TrackerFieldValueParseExplain.cpp` hold the message type and the duration, date and issue-reference parsers.
2. `Source/Core/src/Tracker/TrackerFieldValueParseExplainText.cpp` holds `DescribeParseMessage`. It is the one TU that calls `SmatchetLocalization`, which keeps the parsers free of it.
3. `Source/Core/include/Tracker/TrackerInputNormalisePure.h` + `Source/Core/src/Tracker/TrackerInputNormalisePure.cpp` hold site URLs, executable paths and http(s) URLs.
4. `Source/Core/include/Tracker/JqlQueryValidatePure.h` + `Source/Core/src/Tracker/JqlQueryValidatePure.cpp` hold query validation.

**New — the shared inline error (Ui):**

5. `Source/Core/include/Ui/SmatchetInlineError.h` + `Source/Core/src/Ui/SmatchetInlineError.cpp`.

**Changed — editors that parse:**

6. `Source/Core/src/TicketFieldEditor.cpp`: the grid inline duration editor gets a live overlay error, a canonical commit, and a refusal plus toast on an invalid focus-loss.
7. `Source/Core/src/TicketFieldEditor_Worklog.cpp`, `Source/Core/include/Ui/SmatchetWorklogSubmitPure.h`: the worklog Time spent and Time remaining fields get a live error or preview. The fixed format hint goes, validation becomes tolerant, and the canonical form is sent.
8. `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp`: the generic date picker accepts natural dates. It keeps the typed text while the field is active, draws the inline error, and commits on Enter or deactivation. The grid date picker modal gets a "Type a date" row.
9. `Source/Core/include/Ui/GridSearchInputClassifier.h`: tighter query detection, and pasted issue URLs.
10. `Source/Core/src/Ui/SmatchetGridSearchUi.cpp`: Enter validates the query and refuses a malformed one with a toast that names the problem, instead of replacing the view query. A pasted URL jumps to its issue.
11. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp`: an inline query error under the embedded JQL editor.
12. `Source/Core/src/Tracker/TrackerFieldPayloadPure.cpp`: `ExtractIssueKey` accepts a pasted issue URL.
13. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp`, `Source/Core/src/Ui/SmatchetPreferencesUi_detail.h`: tracker site fields are normalised on deactivation, with a dim "Using …" line. The same normalisation runs on save.
14. `Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp`: p4 and p4vc paths are trimmed and unquoted with an inline error. The AI chat URL must be http(s), with an inline error.
15. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp`: the scalar conflict pane reads `fieldId` from the context. It shows a per-field hint and validates an estimate live with the duration parser; Save is disabled with the reason. The failure message is field-specific.

**Changed — silent failures that now toast:**

16. `Source/Core/src/Ui/P4vLaunch.cpp`, `Source/Core/include/Ui/P4vLaunch.h`: the failure reason is returned.
17. `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp` (three callers) and `Source/Core/src/Ui/SmatchetUserInfoUi_Sections.cpp` (one caller) show the reason: the annotate status line in red, plus a toast.
18. `Source/Core/src/AppController_HostIntegration.cpp`: `OpenUrl` raises a toast with the cause and the fix when it refuses a URL.

**Localization, tests and build lists:**

19. `Source/Core/src/SmatchetLocalization.cpp`: appended `kEntries[]` rows with French.
20. `tests/Core/TrackerFieldValueParseExplain.test.cpp`, `tests/Core/TrackerInputNormalisePure.test.cpp`, `tests/Core/JqlQueryValidatePure.test.cpp` (new): bucket A.
21. `tests/Core/GridSearchInputClassifier.test.cpp`, `tests/Core/TrackerFieldPayloadPure.test.cpp`, `tests/Core/SmatchetWorklogSubmitPure.test.cpp` (changed).
22. `tests/ui/tolerant_parse_inline_error.test.cpp` (new): bucket E. `tests/ui/duration_inline_edit_commit.test.cpp` (changed: new worklog error text).
23. `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt`, `tests/ui/ui_tests_registry.cpp`: appended rows.
24. `docs/plans/active/input-ux-w2-tolerant-parsing.md`: this plan.

The new Core `.cpp` files are picked up by `file(GLOB_RECURSE CORE_SOURCES …)`, so they need no list edit. The new tests are added to `SmatchetTests` and to the Linux `SmatchetTsanTests` subset, so they run in this container.

## Existing utilities reused

- `FormatWorkDurationFromSeconds` (`Source/Core/src/Tracker/TrackerFieldValueParser.cpp`): the canonical duration text that gets committed.
- `TryParseJiraDateTime` / `FormatJiraDateOrDateTimeForApi` (`CompactDateFormat.h`): the canonical date value.
- `TrackerDateTimePure::DaysInMonth` / `IsLeapYear`: calendar validation.
- `TrimCopyAsciiWhitespace` (`StringUtil.h`): trimming.
- `ExtractIssueKeyPrefix` (`ProjectResolver.cpp`) and `ParseGitHubIssueKey` (`GitHubClientHelpers.h`): key shapes, so they are not re-derived.
- `TrackerFieldValueUtils::IsTimeDurationField`: estimate detection in the conflict pane.
- `SmatchetTheme::GetActiveSemanticColors().ErrorText`: the error colour.
- `SmatchetToastManager::Push` / `HasLive`: toasts, deduplicated while one is live.
- `json_safe::ParseBounded`: the conflict context JSON is already parsed through it. No new JSON parse is added.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this plan adds new pure TUs and extracts nothing from an over-cap file.

## UX Pillar callouts

- **Pillar 1 (perf)**: the parsers run once per frame only on the one field being edited, on strings under 64 bytes. They add nothing to the per-cell grid path, because the overlay draws only for the cell in edit. Query validation runs on the dashboard editor buffer, and on the grid search text only on Enter.
- **Pillar 2 (UI never freezes)**: no I/O. The executable-path check is shape-only (trim, quotes, folder vs file). A file-existence probe is deferred (§ Out of scope) because it would be file I/O on the UI thread.
- **Pillar 3 (never crash)**: digit runs are capped before conversion, so there is no `stoll` throw and no signed overflow. Every index is bounds-checked. A fuzz-style table of hostile inputs is part of bucket A.
- **Pillar 4 (accessibility)**: errors are text, not colour alone. The message names the token, and the canonical preview is readable text.
- **Pillar 6 (offline-first)**: there is no new network read. The Enter refusal of a malformed query is local and never discards the view's saved query.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI**: fires, with the grid edit scenario mapped in `scripts/dev/perf-scenario-map.json`. The per-frame cost is limited to the one editing cell.
2. **Pillar 2 static scanner**: N/A. No new sync I/O is reachable from `ImGui::*`.
3. **Dispatcher drain**: N/A. Untouched.
4. **Visible-cue bucket-E harness**: N/A. There is no new stall path.
5. **Marker inventory**: N/A. No new `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: there is no MSVC perf rig in this container. CI's Perf PR-fast lane is the gate.

## Risks / non-goals

- **Risk: what the worklog and grid send changes.** `1.5h` is now sent as `1h 30m`, and a bare `90` as `1h 30m` instead of `90` (90 seconds today). *Mitigation*: this is the intended fix of #38. The canonical text is shown under the field before commit.
- **Risk: a bare number now means minutes, not seconds, in user input.** *Mitigation*: only typed input is affected. Tracker values still go through `ParseWorkDurationToSeconds` unchanged, and the bucket-A table pins both.
- **Risk: a site-URL rewrite drops a legitimate Jira Server context path.** *Mitigation*: only known Jira route segments (`/browse/`, `/secure/`, `/rest/`, `/projects/`, `/jira/software`, …) and query or fragment are cut. Any other path stays. `*.atlassian.net` becomes the bare host. The rewrite is shown before save.
- **Risk: query validation rejects a query the tracker would accept.** *Mitigation*: Enter is refused only on structural errors (unbalanced parens or quotes, a dangling operator). An unknown field is only a warning in the dashboard. The grid search box applies the query anyway.
- **Risk: localization churn.** All keys are new, under the `parse.*` / `input.*` families, with French rows.
- **Risk: dual-target compile.** The new TUs are plain C++14 with no platform headers. `posix-core-check` and the CI dual-target build gate them.
- **Non-goal: a user preference for what a bare number means.** The parser takes `BareNumberUnit`, and the callers pass `Minutes` (Jira's default unit). A preference row is a follow-up.
- **Non-goal: changing `ParseWorkDurationToSeconds` or the sort path.** That is [`duration-parser-unification`](../duration-parser-unification.md).
- **Non-goal: French input words** (`demain`, `ven`). `mar` is both Tuesday in French and March in English, so mixing the two vocabularies is ambiguous.

## Implementation handoff

N/A — a single slice, owned by the orchestrator. The Tracker zone holds the pure layer and Ui holds the widget and editors.

- **Pre-resolved decisions:**
  - Catalog → parser → payload stays intact. The parsers sit in the parser stage, and editors hand the payload builders canonical strings.
  - The grid still commits only through `QueueEdit` and the edit pipeline.
  - No dialog-scope helper is built here; Wave 2 piece 4.1 owns that. Dialog keys keep using `DialogKeysPure.h`.
- **Plan-lock overlap:**
  - `SmatchetLocalization.cpp`, `tests/CMakeLists.txt`, `tests/ui/CMakeLists.txt` and `tests/ui/ui_tests_registry.cpp` are shared with `bulk-edit-rows`, `shared-dropdown-recents` and `input-ux-wave1-palette`. Only rows are appended there.
  - `SmatchetGridHeaderUi.cpp`, `SmatchetNewIssueDraftUi.cpp`, `SmatchetQuickCreateIssueUi.cpp` and `SmatchetViewsDashboardUi_widgets.cpp` are locked by `shared-dropdown-recents` and are not touched (see § Out of scope).

## Verification

- **Bucket A (doctest, `tests/Core/`)**:
  - Accept and reject tables for each parser. Durations: `1.5h`, `90 m`, `1h30`, `1:30`, `2d 4h`, bare `90` as minutes or hours, `0.25m` rounding, `1h 2h`, `-1h`, `1.5`, `3x`, `1:75`, digit-run overflow. Dates: ISO, `11/20`, `20/11`, `20.11.2026`, `Nov 20`, `20 nov 2026`, today / tomorrow / yesterday, weekdays, `next fri`, `+3d`, `-2w`, `in 3 days`, `2/30/2026`, garbage. Issue references: Jira browse URL, `selectedIssue=`, GitHub issue and pull URLs, Linear URL, a lowercase key, a URL with no key.
  - Site URLs: an `atlassian.net` board URL, a server context path, a Plane app URL, a GitHub repo URL, `owner/repo`. Executable paths: surrounding quotes, whitespace, a folder. Chat URL: missing scheme, `ftp:`.
  - Query validation: parens, quotes, a dangling `AND` / `=`, an unknown field.
  - Message text: `DescribeParseMessage` for every id, pinned in English, with the argument substituted and no stray `%`.
  - Classifier: `fix login (again)`, `Q3: roadmap` and `C++ !` are titles. A pasted URL is a `TicketKey`.
  - Worklog submit: tolerant validation.
  - Each test is mutation-checked: the guarded branch is broken on purpose and the test must fail.
- **Bucket E (ImGui Test Engine)**: `tests/ui/tolerant_parse_inline_error.test.cpp`, group `TolerantParse`:
  - (a) Grid duration cell: `1.5x` shows the overlay error, Enter commits nothing, and fixing the text to `1.5h` and pressing Enter commits `1h 30m`.
  - (b) Generic date picker: `tomorrow` resolves and commits. `32/13` shows the inline error and leaves the value alone.
  - (c) Grid date picker "Type a date" row: a bad entry shows the error and Apply is disabled.
  - (d) Worklog: `90 m` previews `= 1h 30m`, and saving sends `1h 30m`.
  - Each case is mutation-checked.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) runs in CI. In this container, `posix-core-check` compiles every Core TU, and `ninja-test-linux` builds and runs the unit subset.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (via `pre-ship.sh`).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: run against the domain terms before the implementation commit. The terms to sharpen are "empty" vs "invalid" vs "zero" for durations, "today" (local, not UTC), and "issue key" vs "issue reference".
- **Manual residue**: none planned. Bucket-E runs on CI's Windows lanes, because this container has no MSVC rig.

## Open questions

None blocking. The bare-number preference and the file-existence probe are recorded follow-ups.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)**: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md` and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **#53 (required-field errors go stale) and #58 (Quick Create gives no reason it is disabled)**: their files (`SmatchetNewIssueDraftUi.cpp`, `SmatchetQuickCreateIssueUi.cpp`) are locked by `shared-dropdown-recents` (PR #2432). Follow-up once it merges.
- **#63, the inline error under the grid search box itself**: the box is drawn in `SmatchetGridHeaderUi.cpp`, which is also locked by that plan. This plan gives the box the Enter refusal and the toast, and gives the dashboard editor the inline line. The under-box line is the follow-up.
- **#82 / #128, the file-existence probe, Browse, Detect and Test (`p4 -V`)**: each needs a worker-thread probe (Pillar 2) and a native file-dialog seam. They are a follow-up; this plan does the trim, unquoting and shape checks.
- **A bare-number unit preference (#38)**: follow-up, as above.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
