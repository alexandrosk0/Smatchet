# Plan — error-message quality pass

> **Slug**: `error-message-quality-pass`
>
> **Status**: `active`

## Context

Track 2 of the user-facing-text session (Track 1: PR #1614, the `user-facing-text-i18n-sweep`
plan on its own branch — the plan-doc link lands when that PR merges).
A full inventory of user-visible error surfaces (tracker/backend failures,
config load, connection failures, Lua errors, AI/update/bug-report/import
surfaces) was built and audited against three questions — does the message say
**what failed**, **why**, and **what the user can do next** — without leaking
internals (tokens, full paths, raw exception text; SECURITY_AUDIT.md context).
The inventory is committed at `docs/guides/error-surface-inventory.md` as the
baseline for future passes. This PR fixes the ~10 worst offenders.

Two structural findings drive most fixes:

1. **Jira has no error-body extractor** (Linear has `ExtractLinearErrorMessage`)
   — worse, seven `JiraIssueMutation.cpp` failure paths append up to 1200 chars
   of the **raw HTTP response body** into the user-facing `.Error` string
   (HTML error pages / JSON blobs straight into a toast).
2. **Raw `ex.what()` / parser text reaches the UI** on five surfaces
   (update-check, offline queue, AI/Whisper test-connection, bug-report submit,
   Jira search parse) — user-meaningless and mildly leaky; the detail belongs in
   the log.

## Approach

- **New pure helper `ExtractJiraErrorMessage(status, body)`**
  (`Tracker/JiraErrorMessagePure.{h,cpp}`, mirroring
  `ExtractLinearErrorMessage`'s contract): bounded parse
  (`json_safe::ParseBoundedOrDiscarded` — the `bare-json-parse-untrusted` rule),
  collect Jira's `errorMessages[]` + `errors{}` values, join + length-cap,
  fall back to `"HTTP <status>"`. All seven mutation failure paths route
  through it; the raw-body appends are removed (the body is already logged via
  `LogTrackerHttpResult`).
- **Exception/parser text off the UI**: each `ex.what()`/`parseErr` surface gets
  a fixed, actionable message; the detail moves to `LOG_WARN`.
- **Localization split per README § Localization**: UI-layer fixed strings route
  through `SmatchetLocalization::T` (new catalog rows, en + fr). Tracker-layer
  `.Error` strings stay English — they are "backend error details … shown
  as-is" by contract, and localizing them would bake a language into strings
  that mix with backend-provided text.
- **Connectivity banner technical suffix**: the appended raw `fetchError`
  (cpr transport text) is scrubbed with the existing
  `smatchet::ai::pure::RedactProviderErrorBody` before display — reuse, not a
  new redactor.
- **Config-parse failure surfaced**: `ConfigManager` records a pending startup
  warning when the main config fails to parse (currently log-only; settings
  silently revert to defaults); the UI toasts it once at startup.

## Files to modify

1. `Source/Core/include/Tracker/JiraErrorMessagePure.h` +
   `Source/Core/src/Tracker/JiraErrorMessagePure.cpp` — NEW pure extractor
   (globbed into `CORE_SOURCES`; strict zone — full lint compliance).
2. `Source/Core/src/Tracker/JiraIssueMutation.cpp` — seven failure paths +
   two parse-failure paths route through the extractor / fixed text.
3. `Source/Core/src/Tracker/JiraIssueSearch.cpp` — parse-failure `.Error`
   strings → fixed "unreadable response" text + logged detail.
4. `Source/Core/src/ConnectivityMonitorService.cpp` — redact the technical
   suffix before embedding.
5. `Source/Core/src/Config/ConfigManager.cpp` (+ header) — pending
   startup-warning seam for a corrupt config file.
6. `Source/Core/src/Ui/SmatchetUI.cpp` — consume the config warning as a toast;
   update-check `ex.what()` → localized message + log.
7. `Source/Core/src/Sync/OfflineQueueService.cpp` — "Cache is not initialized."
   → actionable text; `outError = ex.what()` → fixed text + log.
8. `Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp` — bare `"failed"` fallback →
   actionable message; file-open errors show the file name, not the full path.
9. `Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp` +
   `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp` — test-connection
   `internal: ex.what()` → localized fixed text + log.
10. `Source/Core/src/Diagnostics/BugReportService.cpp` — relay parse/`ex.what()`
    banners → fixed text + log.
11. `Source/Core/src/SmatchetLocalization.cpp` — new rows for the UI-layer
    strings (inserted mid-file, after the audit block, so the hunk cannot
    conflict with Track 1's end-of-array block).
12. `tests/Core/JiraErrorMessagePure.test.cpp` + `tests/CMakeLists.txt` — pure
    extractor coverage (errorMessages / errors{} / fallback / depth-bomb
    discard / length cap).
13. `docs/guides/error-surface-inventory.md` — NEW one-page inventory baseline.

## Existing utilities reused

- `smatchet::json_safe::ParseBoundedOrDiscarded` — `Source/Core/include/Json/BoundedJsonParse.h` — mandatory for the untrusted error body.
- `ExtractLinearErrorMessage` — `Source/Core/src/Tracker/LinearClientHelpers.cpp:191` — the contract the Jira extractor mirrors.
- `smatchet::ai::pure::RedactProviderErrorBody` — `Source/Core/src/AiErrorRedact.cpp` — connectivity-suffix scrub.
- `SmatchetLocalization::T` — UI-layer message localization (Track 1 conventions).

## Extraction sizing

N/A — adds one small pure TU; no split of an over-cap file.

## UX Pillar callouts

- **Pillar 1 (perf)**: extractor runs only on failure paths (no per-frame cost).
- **Pillar 2 (UI blocks)**: no new I/O; all changes are string-shaping on existing paths.
- **Pillar 3 (never crash)**: the new body parse is bounded (`ParseBoundedOrDiscarded`); removes raw-body strings from UI where a pathological body was previously length-capped only.
- **Pillar 4 (accessibility)**: message clarity improves screen-reader/plain-reading comprehension; no layout change.

## Perf-review-system gates

1. **PR-fast CI** — N/A: failure-path string shaping only; no hot-path change.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — untouched.
4. **Visible-cue bucket-E harness** — no new stall path.

## Verification

- `posix-core-check` compile gate + `test-lint-rules.sh --diff origin/develop`
  locally; new doctest for the pure extractor.
- Windows doctest rig + UI buckets: CI (the gate).

## Implementation log

- plan doc + inventory (`docs/guides/error-surface-inventory.md`)
- fixes commit — extractor + ~12 surface fixes + catalog rows + doctest
- review-fixes commit — 3-angle finder pass: AppendCapped underflow/UTF-8 cap
  fix (+ regression tests), audit rows keep the REDACTED body/parse detail the
  toasts no longer carry, `RedactHttpBodyForLog` moved to the cpr-free
  `TrackerHttpPure` TU (TSan-target linkable; the connectivity scrub uses the
  tracker-owned choke point, not a direct AI-subsystem include),
  `FileNameOfPath` hoisted to `StringUtil.h`, the `AiPrefsTestConnection`
  test-connection twin fixed alongside the Preferences UI worker.

## Deviations

- The connectivity-banner scrub can mangle prose that looks like an auth header
  ("Basic authentication…" → "Basic [REDACTED] with…"). Accepted: tracker
  4xx bodies have been observed echoing real `Authorization` headers
  (TrackerHttpPure.h doc), and the 401/403 banner now carries its own
  check-credentials hint, so the redactor's false positive costs less than the
  leak it prevents.
- Test-connection probe twins (`AiPrefsTestConnection.cpp` vs
  `SmatchetPreferencesUi_Assistant.cpp`) received identical fixes under
  `SMATCHET_DEVIATION(rule=duplication)` markers — the twin dedup itself is a
  follow-up (inventory doc gap list).
- Tracker-layer `.Error` strings stay English (README § Localization contract);
  only UI-layer fixed strings gained catalog rows.

## Verification (result)

- `posix-core-check` compile gate green; extractor smoke harness (cap
  underflow boundary, UTF-8 lead-byte cut, basics) green locally.
- `test-lint-rules.sh --diff origin/develop` + `dup_audit.py` clean.
- Windows doctest rig (incl. the new `JiraErrorMessagePure` cases) + UI
  buckets: CI.
