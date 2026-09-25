# Plan — Offline-first: fix the regressions, sweep the class, gate it like DRY

> **Slug**: `offline-first` (matches this file's basename without `.md`).
>
> **Status**: `active` — 13 slices (S1–S13), one PR each, in order. Written for implementation by Claude Haiku 4.5 sessions. Every code anchor was verified against `develop` @ `5b4b1c3` (2026-09-24); line numbers are hints only — locate edits by the quoted text.

## Context

Smatchet must work offline: reads show last-known cached data, writes are saved and replayed on the
next connection. Recent work broke that:
- **The status combo (PR #2234) blocks on a live fetch.** It shows only "Loading transitions…" while the
  fetch runs, which is 1–90 s per issue offline.
- **The comments modal ignores cached comments.** It sits on "Loading comments..." even though the hover
  tooltip already shows the cached thread.

A sweep found about 20 more instances of the same class. The worst predates #2234: an offline Jira
catalog fetch is misclassified as a permanent error, so the app wipes the field catalog, switches the
grid to read-only and drops pending edits.

Nothing in the agent process catches this class, which is why #2234 shipped it. This plan:
1. Fixes every instance.
2. Replaces ~20 hand-rolled `inFlight`/`loaded`/`retryAfter` state machines with **one shared
   offline-aware pattern** (the code-level DRY).
3. Adds an enforced **Quality Pillar 6 — Offline-first**, modelled on Pillar 5 DRY / ADR-0015, with
   lint gates, review rules and an offline test harness (the process-level "don't repeat it").

User decisions:
- **Gate strictness:** split — exact signals block now; heuristics start as warnings and graduate.
- **Offline status options:** remembered ("learned") workflow transitions.
- **Delivery:** PR 1 split into small slices for Haiku.

## Verified root causes (why each slice exists)

1. **Catalog wiped offline (S1).** Chain of events:
   - `Tracker/TrackerFieldCatalog.cpp` `JiraClient::FetchFieldCatalog` turns every failure, transport
     included, into `TrackerErrorUnknown`.
   - `Ui/SmatchetUI.cpp` sets `ErrorTransient = IsRetryable() = false`.
   - `AppController::HandleFieldCatalogError` takes its non-transient branch and clears the catalog.
   - The banner goes to `Error`, `readOnlyMode` turns on, and `EnqueueGridFieldEdits` clears the
     pending edits. `SetAvailableUsers({})` also wipes users.
   - Linear's team lookup (`ResolveCatalogTeamId`) has the same flaw.
2. **Status combo (S5).** `TicketFieldEditor.cpp` `RenderSingleSelectEditor` shows no options until
   `IssueTransitionsCacheService` finishes. In that service:
   - a failure is cached as final and never reset on reconnect;
   - nothing is persisted;
   - there is no exception guard;
   - non-Jira backends log a warning per issue.
3. **Status never offline-queueable (S6).** `FieldEditPipelineService::FieldEditSupportsOfflineQueue`
   omits `TrackerFieldFamily::Status`. The grid also always tries the network first and does a second
   blocking editmeta fetch before queueing, so an offline edit sits in RAM for 2–3 retry windows.
   Linear update failures are all `InvalidRequest`, so they are never queued.
4. **Comments modal (S7).** `Ui/SmatchetCommentsModalUi.cpp` has four problems:
   - it opens with an empty list and always fetches from the network;
   - it shows only "Loading comments..." while waiting;
   - on failure it shows "No comments yet." even though the cached thread is in memory;
   - its in-flight flag is set before the launch and is never cleared if the worker throws.
   It also shows a false "Comment Queued" toast.
5. **Writes that bypass the queue (S8–S10).** Comment post, worklog, watch, Annotate, the
   command/MCP/Lua field edits (`SubmitFieldEdit` never queues), sprint/estimate edits, and bulk
   import (no `QueueCreateOffline` fallback).
6. **Reads with no offline copy (S11–S13).** Per-project components ("Loading components…" forever,
   raw ids), users, editmeta (a fetch storm every frame on failure), the project picker ("No projects
   found."), Annotate user lookup ("Past Employee" when offline), the Views Fields tab, attachments,
   watchers/votes and User Info.

## Implementer contract (every Haiku session reads this first)

1. **One slice per session, one PR per slice, base `develop`.** Use the branch your session designates.
   Only start slice N after slice N−1 is merged. Verify with `git log origin/develop --oneline | head`,
   which must show the previous slice's PR title.
2. **Read before editing:** `AGENTS.md`, plus the `AGENTS.md` of every `Source/Core/src/<ctx>/` you
   touch (Tracker, Sync, Persistence, Ui), plus this slice.
3. **Anchors:** find each edit by searching for the exact quoted text (`grep -n -F`). If an anchor is
   missing, or matches more than once where the slice says "unique", **STOP** and report which anchor
   failed and what you found (AI_POLICY § Escalate, don't assume). Never guess an equivalent location.
4. **Scope:** touch only the files in the slice's **Files** table. No drive-by refactors, renames or
   reformatting of untouched code. If you believe another file must change, STOP and report.
5. **C++ rules:**
   - C++14 only: no `std::optional`/`string_view`/`variant`, no structured bindings, no `if constexpr`.
   - Logging only through `LOG_*`. No raw `new`/`delete`. Pass non-trivial parameters by `const&`.
   - **Never add `#include "AppController.h"` to a new file** (fan-in gate).
   - **Never add `#include "Ui/…"` under `Tracker/`, `Sync/`, `Persistence/` or `Config/`.**
   - A header directly in `Source/Core/include/` may include only rank-0 headers (root, `Types/`,
     `Interfaces/`) or `Tracker/TrackerError.h` / `Tracker/TrackerFieldSchema.h`.
6. **Comments:**
   - No `// ----` banner lines, and no runs of 2+ bare `//` lines.
   - Comment what the code does and why. Never write `PR <n>`.
   - `SMATCHET_DEVIATION(...)` markers must be a single line directly above the target.
   - In markdown, `AGENTS.md § <Section>` must name a heading of the root `AGENTS.md` or of
     `docs/agent-rules/*.md` (CI `test-doc-anchors` fails otherwise). For a leaf doc, write
     'the "<Section>" section of `Source/Core/src/<ctx>/AGENTS.md`' instead.
7. **Size limits:** functions ≤ 120 lines (≤ 200 for `Draw*`/`Render*` or anything under `Ui/`),
   ≤ 30 branches. Source files stay < 67 KB. `AGENTS.md` stays ≤ 150 lines.
8. **Registering a new test `tests/Core/X.test.cpp`:** list it in `tests/CMakeLists.txt`, or configure
   fails.
   - **Always** add it to the `SmatchetTests` list, which is what PR CI runs on Windows. Put it on the
     line after `    Core/CommentBlobFormatPure.test.cpp` (4-space indent, ~line 1163).
   - **Also** add it to the `SmatchetTsanTests` list, but only if the test and everything it links are
     free of ImGui/cpr/SQLite/httplib. Put it on the line after
     `        Core/CommentBlobFormatPure.test.cpp` (8-space indent, ~line 283). Also list any production
     `.cpp` it needs there, as `"${CMAKE_SOURCE_DIR}/Source/Core/src/<file>.cpp"`.
   - A new production `.cpp` that an existing test target links must be added next to its siblings in
     that target's list. Find them with `grep -n "<SiblingFile>.cpp" tests/CMakeLists.txt`.
9. **Local verification (Linux cloud container).** Run all of these, in order:
   - `bash scripts/dev/pre-ship.sh origin/develop` — clang-format, `test-lint-rules.sh --diff`, the md
     check and the test-list check.
   - `bash agents/scripts/project/test-lint-rules.sh --selftest`
   - Compile every core TU:
     `cmake --preset posix-core-check -DSMATCHET_BUILD_TESTS=ON && cmake --build --preset posix-core-check --target SmatchetCore_PosixCheck -- -k 0`
   - Linux doctests:
     `cmake --preset ninja-test-linux && cmake --build --preset ninja-test-linux && build/ninja-test-linux/tests/SmatchetTsanTests --test-case='<filters given in the slice>'`
   - If the slice touches `agents/scripts/**` or `tests/bats/**`, also run
     `bash agents/scripts/project/test-lint-rules-bats.sh`. It needs `bats`. If `command -v bats` fails,
     run `git clone --depth 1 https://github.com/bats-core/bats-core "$HOME/bats-core" && "$HOME/bats-core/install.sh" "$HOME/.local" && export PATH="$HOME/.local/bin:$PATH"`.
     If that also fails, say "bats not run: <reason>" in the PR body.
   - The Windows-only suites (full `SmatchetTests`, ASan/UBSan, bucket-E UI tests) run in PR CI. Wait
     for them to go green.
10. **Tracking Issue.** Every product slice starts with this step (S1–S3 and S5–S13; S4 has none):
    - Search open Issues for the symptom first.
    - If none matches, create `bug(<area>): <title>` with the labels given in the slice, using the
      GitHub MCP issue tool (or `gh issue create` where available).
    - The body contains: symptom · trigger ("tracker unreachable") · `file:line` · cause · "found by
      offline-first sweep".
    - Put `Fixes #<n>` in the PR body.
11. **PR:**
    - Title = the slice title.
    - Body: link to this plan + slice id, `Fixes #n`, the list of tests added, the verification output
      summary, and "Visual: <bucket-E test name>" for `[visual]` slices.
    - Ship with the AGENTS.md ship-loop (auto-merge applies on green gates).
12. **`[visual]` slices** touch `Smatchet*Ui*.cpp` / `SmatchetLocalization.cpp`. Their named bucket-E
    test is the visual validation. If that CI job is not green, **do not merge**: pause and ask the user
    (ship-loops § Visual-validation exception).
13. **STOP and escalate when:**
    - an anchor fails;
    - a gate fails for a reason your diff does not explain;
    - a test you did not touch breaks and the slice does not name it;
    - you would exceed a size cap;
    - you need a file outside the list.
14. **After merge:** append 3–6 lines under your slice id in `## Out of scope (flagged, not designed)

Offline editing/deleting of existing comments; offline issue search; the three Pillar-2 debt items S4
files (per-frame pending-count SELECTs, UI-thread `SaveTicket`, per-frame project-picker cache read).

## Implementation log` of
    `docs/plans/active/offline-first.md`, covering what shipped, any deviations and the PR link. Use a
    docs-only follow-up commit on your next slice's branch, or on the same PR before merge.

## Slice index

| # | Title (PR title) | Depends | Issue labels |
|---|---|---|---|
| S1 | fix(catalog): keep the field catalog and pending edits when the tracker is unreachable | — | bug, P1, area:tracker-backend, src:code-review |
| S2 | feat(offline): shared offline-first primitives (connectivity helpers, KeyedLookupCache, freshness cue) | S1 | bug, P2, area:offline-sync, src:code-review |
| S3 | test(offline): fake-network harness + OfflineFirst bucket-E lane | S2 | bug, P2, area:build, src:code-review |
| S4 | docs(gates): Quality Pillar 6 offline-first — ADR-0026, lint gates, review + agent rules | S3 | (none; process) |
| S5 | fix(status): status combo never blocks offline; remember valid transitions `[visual]` | S4 | bug, P1, area:grid-engine, src:code-review |
| S6 | fix(field-edit): queue field edits immediately when offline; status edits are queueable | S5 | bug, P1, area:offline-sync, src:code-review |
| S7 | fix(comments): comments modal shows cached thread offline; truthful posting state `[visual]` | S6 | bug, P1, area:ui, src:code-review |
| S8 | feat(offline): pending-action queue; comments posted offline replay on reconnect `[visual]` | S7 | bug, P2, area:offline-sync, src:code-review |
| S9 | fix(offline): worklogs, watch and Annotate comments go through the pending-action queue | S8 | bug, P2, area:offline-sync, src:code-review |
| S10 | fix(offline): command/MCP/Lua/Annotate field edits, sprint/estimate and bulk import queue offline | S9 | bug, P2, area:command-system, src:code-review |
| S11 | fix(offline): persist component options, users and edit permissions for offline use `[visual]` | S10 | bug, P2, area:tracker-backend, src:code-review |
| S12 | fix(offline): project picker, Views Fields tab, Annotate lookup and pane catalog work offline `[visual]` | S11 | bug, P2, area:ui, src:code-review |
| S13 | fix(offline): attachment disk cache, watchers/votes and User Info offline states `[visual]` | S12 | bug, P3, area:ui, src:code-review |

---

## S1 — fix(catalog): keep the field catalog and pending edits when the tracker is unreachable

**Symptom for the Issue:** "Offline Jira catalog refresh is classified as a permanent error. The
catalog is cleared, the grid turns read-only and pending edits are discarded."

**Files**

| File | Change |
|---|---|
| `docs/plans/active/offline-first.md` | NEW — copy of this plan (first commit `wip(plan): offline-first`) |
| `Source/Core/include/CatalogOfflinePolicyPure.h` | NEW pure header |
| `Source/Core/src/Tracker/TrackerFieldCatalog.cpp` | classify every failure exit |
| `Source/Core/include/Tracker/JiraClient.h` | add a defaulted `outClassified` param |
| `Source/Core/src/Tracker/LinearClient.cpp` | classify the team-lookup failure |
| `Source/Core/src/AppController_CatalogAndFieldEdit.cpp` | rewrite `HandleFieldCatalogError` (never clears) |
| `Source/Core/src/Ui/SmatchetUI.cpp` | stop wiping users on a failed fetch |
| `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` | hold (not drop) pending edits under banner read-only |
| `Source/Core/src/ConnectivityMonitorService.cpp` | banner text matching via the new header |
| `tests/Core/CatalogOfflinePolicyPure.test.cpp` | NEW |
| `tests/Core/TrackerCatalogBuild.test.cpp` | update 1 case, add 2 |
| `tests/Core/ConnectivityMonitorService.test.cpp` | add 1 case |
| `tests/CMakeLists.txt` | register the new test (both lists) |

### S1 steps

**Step 0 — plan doc (already done).** The planning session committed this file as `wip(plan): offline-first` on the S1 branch. Verify `docs/plans/active/offline-first.md` exists; do not recreate it. After S1 merges, append to its `## Implementation log` (contract §14).

**Step 1 — create `Source/Core/include/CatalogOfflinePolicyPure.h`** with exactly this content:
```cpp
#pragma once

// CatalogOfflinePolicyPure — Quality Pillar 6 (offline-first) decisions for a failed field-catalog
// refresh. Pure (no I/O, no Logger.h) so the doctest rig links it bare.
//
// Invariant: a failed refresh never discards a catalog the user already has, whether in memory or in
// the local snapshot. Only the banner differs: a retryable failure (Transport / RateLimited /
// ServerError) shows a Warning; a non-retryable one (Auth / InvalidRequest / Parse / Unknown) shows an
// Error because the user has to act.

#include <cstring>
#include <string>

namespace smatchet {
namespace catalogoffline {

enum class CatalogFailureBanner : unsigned char {
    WarningUsingCached,       ///< retryable; the in-memory catalog is kept
    WarningRestoredSnapshot,  ///< retryable; memory was empty and the disk snapshot was restored
    WarningSessionHadCatalog, ///< retryable; nothing to restore but this session had a catalog
    ErrorKeepCatalog,         ///< non-retryable; the catalog (memory or snapshot) is kept
    ErrorNoCatalog,           ///< nothing available at all
};

inline CatalogFailureBanner DecideCatalogFailureBanner(bool errorTransient, bool hasFieldsNow, bool snapshotLoaded,
                                                       bool everLoaded) {
    if (!errorTransient) {
        return (hasFieldsNow || snapshotLoaded) ? CatalogFailureBanner::ErrorKeepCatalog
                                                : CatalogFailureBanner::ErrorNoCatalog;
    }
    if (hasFieldsNow) {
        return CatalogFailureBanner::WarningUsingCached;
    }
    if (snapshotLoaded) {
        return CatalogFailureBanner::WarningRestoredSnapshot;
    }
    if (everLoaded) {
        return CatalogFailureBanner::WarningSessionHadCatalog;
    }
    return CatalogFailureBanner::ErrorNoCatalog;
}

/// Every catalog warning AppController builds embeds the raw fetch error after this marker.
constexpr const char* kLastFetchFailedMarker = " Last fetch failed: ";
/// Prefix of the warning set when the catalog was restored from the snapshot at startup.
constexpr const char* kWorkingOfflinePrefix = "Working offline:";

inline bool IsStartupSnapshotWarning(const std::string& warning) {
    const std::size_t n = std::strlen(kWorkingOfflinePrefix);
    return warning.size() >= n && warning.compare(0, n, kWorkingOfflinePrefix) == 0;
}

/// The raw fetch error embedded in a catalog warning, or "" when the warning carries none.
inline std::string ExtractLastFetchFailedDetail(const std::string& warning) {
    const std::size_t pos = warning.find(kLastFetchFailedMarker);
    if (pos == std::string::npos) {
        return std::string();
    }
    return warning.substr(pos + std::strlen(kLastFetchFailedMarker));
}

} // namespace catalogoffline
} // namespace smatchet
```

**Step 2 — `Tracker/TrackerFieldCatalog.cpp`.** Four edits:

2a. Replace the helper signature. Anchor (unique):
```cpp
bool FetchAndParseFieldList(const std::string& base, const cpr::Header& headers, std::vector<TrackerField>& outFields,
                            std::vector<std::string>& outSprintFieldIds, std::string& outError) {
```
Replace it with:
```cpp
bool FetchAndParseFieldList(const std::string& base, const cpr::Header& headers, std::vector<TrackerField>& outFields,
                            std::vector<std::string>& outSprintFieldIds, std::string& outError,
                            TrackerError& outClassified) {
```

2b. Replace the HTTP failure block. Anchor (unique):
```cpp
    if (fieldsResponse.status_code != 200) {
        outError = "Failed to fetch fields: HTTP " + std::to_string(fieldsResponse.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }
```
Replace it with:
```cpp
    if (fieldsResponse.status_code != 200) {
        outError = "Failed to fetch fields: HTTP " + std::to_string(fieldsResponse.status_code);
        if (fieldsResponse.status_code <= 0 && !fieldsResponse.error.message.empty()) {
            outError += " (" + fieldsResponse.error.message + ")";
        }
        // Status 0 (no response) classifies as Transport, so callers keep the cached catalog.
        outClassified = ClassifyRejectedHttpStatus(fieldsResponse.status_code, outError);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }
```

2c. In the same function, find each of these three statements. Immediately after each one, before its
`LOG_ERROR`, insert the line `        outClassified = TrackerErrorParse(outError);`. Use the correct
indentation: 12 spaces for the first two, 8 for the third.
- `outError = "Failed to parse /field response: " + parseErr;`
- `outError = "Unexpected /field response shape.";`
- `outError = std::string("Failed to parse /field response: ") + ex.what();`

2d. Bool overload. Replace the signature, whose anchor starts `bool JiraClient::FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,`,
through `std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError) {` with:
```cpp
bool JiraClient::FetchFieldCatalog(const TrackerConfig& cfg, const std::string& projectKey,
                                   std::vector<TrackerField>& outFields, std::vector<TrackerComponent>& outComponents,
                                   std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError,
                                   TrackerError* outClassified) {
```
Then make three replacements in the same function:
- Replace `    if (!EnsureTrackerAuthConfig(cfg, outError)) {\n        return false;\n    }` with:
  ```cpp
      if (!EnsureTrackerAuthConfig(cfg, outError)) {
          if (outClassified) {
              *outClassified = TrackerErrorInvalidRequest(outError);
          }
          return false;
      }
  ```
- Replace `    if (!FetchAndParseFieldList(base, headers, outFields, sprintFieldIds, outError)) {` with:
  ```cpp
      TrackerError listClassified;
      if (!FetchAndParseFieldList(base, headers, outFields, sprintFieldIds, outError, listClassified)) {
          if (outClassified) {
              *outClassified = listClassified;
          }
  ```
  Keep the existing `        return false;\n    }` lines that follow.
- In the Result overload, replace:
  ```cpp
      if (!FetchFieldCatalog(cfg, projectKey, fields, components, issueTypeMeta, outError)) {
          // TODO(#21b later slice): re-thread status from inner helper instead of collapsing to Unknown — IsRetryable()
          // consumers land in a later slice.
          return Result<TrackerFieldCatalogResult, TrackerError>::Err(TrackerErrorUnknown(std::move(outError)));
      }
  ```
  with:
  ```cpp
      TrackerError classified;
      if (!FetchFieldCatalog(cfg, projectKey, fields, components, issueTypeMeta, outError, &classified)) {
          // Keep the failure's kind (Transport stays retryable) so an offline refresh never reads as permanent.
          return Result<TrackerFieldCatalogResult, TrackerError>::Err(
              classified.IsOk() ? TrackerErrorUnknown(std::move(outError)) : classified);
      }
  ```

**Step 3 — `Tracker/JiraClient.h`.** Anchor (unique):
`                           std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError);`
Replace it with:
`                           std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError, TrackerError* outClassified = nullptr);`
Then run clang-format on the file.

**Step 4 — `Tracker/LinearClient.cpp`.**
- In `ResolveCatalogTeamId`, add a trailing parameter `TrackerError* outClassified = nullptr` to the
  definition. The function has no separate declaration; if `grep -n "ResolveCatalogTeamId" Source`
  shows one, update it too.
- In its failure `if (resp.status_code != 200 || parsed.is_discarded() || ...) {` block, add as the
  first statement:
  ```cpp
          if (outClassified && resp.status_code != 200) {
              *outClassified = ClassifyRejectedHttpStatus(resp.status_code, "Linear team lookup failed (HTTP " +
                                                                             std::to_string(resp.status_code) + ")");
          }
  ```
- In `FetchFieldCatalog`, replace:
  ```cpp
      const std::string teamId =
          ResolveCatalogTeamId(auth.ApiUrl, auth.ApiKey, cfg.LinearTeamId, cfg.LinearTeamKey, projectKey);
      if (teamId.empty()) {
  ```
  with:
  ```cpp
      TrackerError teamClassified;
      const std::string teamId = ResolveCatalogTeamId(auth.ApiUrl, auth.ApiKey, cfg.LinearTeamId, cfg.LinearTeamKey,
                                                      projectKey, &teamClassified);
      if (teamId.empty() && !teamClassified.IsOk()) {
          return CatalogResult::Err(teamClassified);
      }
      if (teamId.empty()) {
  ```
  The existing InvalidRequest return stays for the genuinely unconfigured case.

**Step 5 — `AppController_CatalogAndFieldEdit.cpp`.**
- Add `#include "CatalogOfflinePolicyPure.h"` next to the other quote includes, alphabetically.
- Replace the **entire** function `void AppController::HandleFieldCatalogError(...)` (≈ lines 386-486,
  from its signature to its closing `}`) with:
```cpp
void AppController::HandleFieldCatalogError(const std::string& error, bool errorTransient,
                                            const std::string& catalogCacheKey, bool catalogPlane) {
    // Latch the catalog once: fieldCatalog() re-resolves focusedContextPtr_ per call; a focus
    // switch between two calls would lock context A's mutex while mutating context B (Pillar 3).
    GridContextFieldCatalog& cat = fieldCatalog();
    bool hasFieldsNow;
    {
        std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
        hasFieldsNow = !cat.AvailableFields.empty();
    }
    // Pillar 6 (offline-first): a failed refresh never clears a catalog the user already has. When
    // memory is empty, restore the local snapshot whatever the error kind; only the banner differs.
    bool snapshotLoaded = false;
    std::string snapErr;
    if (!hasFieldsNow) {
        std::vector<TrackerField> snapFields;
        std::vector<TrackerComponent> snapComponents;
        std::vector<TrackerIssueTypeCreateMeta> snapIssueTypeMeta;
        snapshotLoaded = FieldCatalogCache::TryLoadFieldCatalogSnapshot(catalogCacheKey, snapFields, snapComponents,
                                                                        snapIssueTypeMeta, snapErr);
        if (snapshotLoaded) {
            {
                std::lock_guard<std::mutex> lk(cat.availableFieldsMutex_);
                cat.AvailableFields = std::move(snapFields);
                cat.AvailableComponents = std::move(snapComponents);
                cat.AvailableIssueTypeMeta = std::move(snapIssueTypeMeta);
                if (!catalogPlane) {
                    for (auto& field : cat.AvailableFields) {
                        if (IsNonEditableTimetrackingFieldId(field.Id)) {
                            field.ReadOnly = true;
                        }
                    }
                }
            }
            cat.fieldCatalogEverLoaded_ = true;
            if (!catalogPlane) {
                EraseCatalogLegacyCommentField(cat);
                EnsureCatalogHistoryField(cat);
                EnsureCatalogCommentsField(cat);
            }
        }
    }
    const std::string backendLabel = catalogPlane ? "Plane" : "Jira";
    using smatchet::catalogoffline::CatalogFailureBanner;
    switch (smatchet::catalogoffline::DecideCatalogFailureBanner(errorTransient, hasFieldsNow, snapshotLoaded,
                                                                  cat.fieldCatalogEverLoaded_)) {
    case CatalogFailureBanner::WarningUsingCached: {
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        const std::string nextWarning =
            "Offline: using cached " + backendLabel + " field catalog. Last fetch failed: " + error;
        if (nextWarning != cat.LastTrackerFieldCatalogWarning) {
            cat.LastTrackerFieldCatalogWarning = nextWarning;
            cat.TrackerFieldCatalogRevision.fetch_add(1);
        }
        LOG_WARN("AppController::SetFieldCatalog transport failure (catalog preserved): %s", error.c_str());
        return;
    }
    case CatalogFailureBanner::WarningRestoredSnapshot:
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning =
            "Offline: restored " + backendLabel + " field catalog from local snapshot. Last fetch failed: " + error;
        LOG_WARN("AppController::SetFieldCatalog transport failure; loaded snapshot err=%s", snapErr.c_str());
        break;
    case CatalogFailureBanner::WarningSessionHadCatalog:
        cat.LastTrackerFieldCatalogError.clear();
        cat.LastTrackerFieldCatalogErrorTransient = false;
        cat.LastTrackerFieldCatalogWarning =
            "Offline: no field catalog snapshot could be loaded for this tracker context. Last fetch failed: " + error;
        LOG_WARN("AppController::SetFieldCatalog transport failure; no snapshot (session had catalog): %s",
                 error.c_str());
        break;
    case CatalogFailureBanner::ErrorKeepCatalog:
        // Non-retryable (auth / config / parse): the user must act, so show the error banner, but keep
        // the catalog. The grid holds pending edits instead of discarding them.
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogError = error;
        cat.LastTrackerFieldCatalogErrorTransient = false;
        LOG_ERROR("AppController::SetFieldCatalog error (catalog kept): %s", error.c_str());
        break;
    case CatalogFailureBanner::ErrorNoCatalog:
        cat.fieldCatalogEverLoaded_ = false;
        cat.LastTrackerFieldCatalogWarning.clear();
        cat.LastTrackerFieldCatalogErrorTransient = errorTransient;
        cat.LastTrackerFieldCatalogError =
            errorTransient ? "No cached " + backendLabel + " field catalog available. " +
                                 (error.empty() ? std::string("Last fetch failed.") : error)
                           : error;
        LOG_ERROR("AppController::SetFieldCatalog error (no cache): %s", error.c_str());
        break;
    }
    cat.TrackerFieldCatalogRevision.fetch_add(1);
}
```

**Step 6 — `Ui/SmatchetUI.cpp`.** Anchor (unique as a block):
```cpp
                app.SetFieldCatalog({}, {},
                                    result.Error.empty() ? std::string("Failed to fetch field catalog.") : result.Error,
                                    result.ErrorTransient);
                app.SetAvailableUsers({});
                d.fieldCatalogWarning.clear();
```
Delete **only** the line `                app.SetAvailableUsers({});` in this block. Leave the backend-switch
`SetAvailableUsers({})` near line 706 unchanged.

**Step 7 — `SmatchetGridFieldEditPipeline.cpp`.** Anchor (unique):
```cpp
        if (readOnlyMode) {
            d.queuedFieldEdits.clear();
        }
```
Replace it with:
```cpp
        // Pillar 6: only the user's own Read-only preference discards not-yet-sent edits. A tracker
        // error banner also makes the grid read-only, but those edits are held (the pump does not
        // dispatch while read-only) and go out once the tracker is usable again.
        if (readOnlyMode && d.cfg.ReadOnlyMode) {
            d.queuedFieldEdits.clear();
        }
```

**Step 8 — `ConnectivityMonitorService.cpp`.**
- Add `#include "CatalogOfflinePolicyPure.h"`.
- Delete the two lines that start `constexpr char kWorkingOfflineSnapshotCatalog[] =`.
- In `CatalogOfflineTechnicalSuffix`, replace everything from `    static const char* prefixes[] = {`
  through the `if (cw == kWorkingOfflineSnapshotCatalog) {\n        return std::string();\n    }` block.
  The file has two `static const char* prefixes[] = {` lines; use the **first** (≈ line 239), inside
  `CatalogOfflineTechnicalSuffix`, and leave the one in `TicketOfflineTechnicalSuffix` alone. Replace
  it with:
```cpp
    // Every AppController catalog warning embeds the raw fetch error after one shared marker
    // (CatalogOfflinePolicyPure.h), so match the marker rather than per-wording prefixes.
    const std::string detail = smatchet::catalogoffline::ExtractLastFetchFailedDetail(cw);
    if (!detail.empty()) {
        // The suffix embeds the raw fetch error (cpr transport / backend text) — scrub
        // secret-shaped tokens before it reaches the banner (reuses the AI-side redactor).
        return RedactHttpBodyForLog(detail);
    }
    if (smatchet::catalogoffline::IsStartupSnapshotWarning(cw)) {
        return std::string();
    }
```
- Replace `    const bool snapshotCatalogOnly = haveCw && cw == kWorkingOfflineSnapshotCatalog;` with
  `    const bool snapshotCatalogOnly = haveCw && smatchet::catalogoffline::IsStartupSnapshotWarning(cw);`.
- If `grep -n "std::strlen\|<cstring>" Source/Core/src/ConnectivityMonitorService.cpp` shows `strlen`
  is no longer used, leave the include alone. Do not remove includes.

**Step 9 — tests.**
- NEW `tests/Core/CatalogOfflinePolicyPure.test.cpp`. It includes `"CatalogOfflinePolicyPure.h"` and
  `<doctest/doctest.h>` and has these cases:
  - `TEST_CASE("DecideCatalogFailureBanner never chooses a wipe for a retryable failure")`: checks the
    four transient combinations → `WarningUsingCached` (fields), `WarningRestoredSnapshot`
    (snapshot only), `WarningSessionHadCatalog` (neither but everLoaded), `ErrorNoCatalog` (nothing).
  - `TEST_CASE("DecideCatalogFailureBanner keeps the catalog on a non-retryable failure")`:
    `(false,true,false,false)` → `ErrorKeepCatalog`; `(false,false,true,false)` → `ErrorKeepCatalog`;
    `(false,false,false,true)` → `ErrorNoCatalog`.
  - `TEST_CASE("ExtractLastFetchFailedDetail reads every AppController warning wording")`: for each of
    these three strings, assert the result is `"HTTP 0 (Couldn't connect)"`:
    - `"Offline: using cached Jira field catalog. Last fetch failed: HTTP 0 (Couldn't connect)"`
    - `"Offline: restored Plane field catalog from local snapshot. Last fetch failed: HTTP 0 (Couldn't connect)"`
    - `"Offline: no field catalog snapshot could be loaded for this tracker context. Last fetch failed: HTTP 0 (Couldn't connect)"`
    Also `ExtractLastFetchFailedDetail("Working offline: tracker field catalog loaded from local snapshot until a live refresh succeeds.")` is empty.
  - `TEST_CASE("IsStartupSnapshotWarning matches every startup wording")`: true for the "tracker",
    "Plane" and "Jira" `Working offline:` strings; false for `"Offline: using cached Jira ..."` and for
    `""`.
  - Register it in both lists (contract §8).
- `tests/Core/TrackerCatalogBuild.test.cpp`. In the case `FetchFieldCatalog result overload — /field failure returns an Err with a TrackerError`:
  - Replace the three comment lines plus `CHECK(catalogResult.error().Kind == TrackerErrorKind::Unknown);` with:
    ```cpp
        // The failure keeps its HTTP kind: 500 is a retryable ServerError, never a collapsed Unknown.
        CHECK(catalogResult.error().Kind == TrackerErrorKind::ServerError);
        CHECK(catalogResult.error().IsRetryable());
    ```
  - Add two cases after it:
    - `TEST_CASE("FetchFieldCatalog result overload — 401 on /field classifies as Auth")`:
      `fx.ScriptStatus("/rest/api/3/field", 401);`, then
      `CHECK(catalogResult.error().Kind == TrackerErrorKind::Auth); CHECK_FALSE(catalogResult.error().IsRetryable());`.
    - `TEST_CASE("FetchFieldCatalog result overload — unreachable host classifies as Transport")`:
      `JiraCatalogHttpFixture fx; TrackerConfig cfg = fx.Config(); cfg.Domain = "http://127.0.0.1:1";`
      then call `client.FetchFieldCatalog(cfg, std::string())` and
      `REQUIRE_FALSE(catalogResult); CHECK(catalogResult.error().Kind == TrackerErrorKind::Transport); CHECK(catalogResult.error().IsRetryable());`.
- `tests/Core/ConnectivityMonitorService.test.cpp`. After the case
  `GetBannerForUi snapshot-catalog headline (Warning)`, add
  `TEST_CASE("ConnectivityMonitorService::GetBannerForUi startup snapshot wording from AppController is recognised")`.
  It follows the same shape with `CatalogWarningImpl = "Working offline: tracker field catalog loaded from local snapshot until a live refresh succeeds."`
  and checks `Warning` plus `banner.Message.find("local snapshot") != std::string::npos`.

**Verify**
- Linux: `--test-case='DecideCatalogFailureBanner*,ExtractLastFetchFailedDetail*,IsStartupSnapshotWarning*,ConnectivityMonitorService*'`.
- CI: `TrackerCatalogBuild` (Windows), plus the existing bucket-E `JiraDeterministic` tests.

**Done when:**
- all gates are green;
- `grep -n "TODO(#21b" Source/Core/src/Tracker/TrackerFieldCatalog.cpp` is empty;
- the new tests pass.

---

## S2 — feat(offline): shared offline-first primitives

**Symptom for the Issue:** "Offline-aware lookups are hand-rolled per feature; a failure can latch a
loader forever and connectivity state is read unsafely off the UI thread (`sync.tracker_status`)."

**Files**

| File | Change |
|---|---|
| `Source/Core/include/OfflineFirstPure.h` | NEW |
| `Source/Core/include/ScopeExit.h` | NEW (moved from OfflineQueueService.cpp) |
| `Source/Core/include/KeyedLookupCache.h` | NEW |
| `Source/Core/include/DataFreshnessCue.h`, `Source/Core/src/DataFreshnessCue.cpp` | NEW |
| `Source/Core/src/SmatchetLocalization.cpp` | 7 keys |
| `Source/Core/src/Sync/OfflineQueueService.cpp` | use shared ScopeExit |
| `Source/Core/include/ConnectivityMonitorService.h` | atomic `lastState_`, `RequestProbeNow` |
| `Source/Core/include/Interfaces/IAppSync.h` | inline `IsTrackerOffline()` |
| `Source/Core/include/AppController.h`, `Source/Core/src/AppController_Connectivity.cpp` | `RequestTrackerProbeNow()` |
| `Source/Core/include/IEditMetaDeps.h`, `Source/Core/include/IFieldEditDeps.h` | `TrackerConnectivity()` |
| `Source/Core/include/GridContextDepsAdapter.h`, `Source/Core/src/GridContextDepsAdapter.cpp` | implement it |
| `tests/support/FakeEditMetaDeps.h`, `tests/support/FakeFieldEditDeps.h` | implement it |
| `tests/Core/OfflineFirstPure.test.cpp`, `tests/Core/KeyedLookupCache.test.cpp` | NEW |
| `tests/CMakeLists.txt` | register both (both lists) |

### S2 steps

**Step 1 — `Source/Core/include/OfflineFirstPure.h`:**
```cpp
#pragma once

// OfflineFirstPure — Quality Pillar 6 (offline-first) decisions shared by every network-backed read and
// every tracker write. Pure: no I/O, no Logger.h, no ImGui. See docs/agent-rules/quality-pillars.md § 6.

#include "Types/ConnectivityTypes.h"

#include <chrono>
#include <cstdint>

namespace smatchet {
namespace offline {

using Clock = std::chrono::steady_clock;

/// Backoff after a failed lookup fetch before the next attempt (matches the components loader).
constexpr int kLookupRetryAfterSeconds = 30;

/// True while the last connectivity probe says the tracker cannot be reached.
inline bool IsOfflineState(TrackerConnectivityState s) {
    return s == TrackerConnectivityState::TransportDown || s == TrackerConnectivityState::ServiceUnavailable;
}

/// A fetch may start only when the tracker is not known to be offline and any backoff has passed.
inline bool ShouldAttemptNetwork(TrackerConnectivityState s, Clock::time_point now, Clock::time_point retryAfter) {
    return !IsOfflineState(s) && now >= retryAfter;
}

enum class DataFreshness : std::uint8_t {
    Fresh,              ///< live data from this session
    Refreshing,         ///< cached data shown while a live fetch runs
    CachedOffline,      ///< cached data shown; the tracker is unreachable
    CachedStale,        ///< cached data shown; the last live fetch failed or has not run yet
    LoadingNoCache,     ///< nothing cached yet; a live fetch is running
    UnavailableNoCache, ///< nothing cached and no fetch running
};

struct FreshnessInputs {
    bool HasCache = false;          ///< a value (live or restored) is available to render
    bool Live = false;              ///< the value came from a successful fetch in this session
    bool InFlight = false;          ///< a fetch is running now
    bool LastAttemptFailed = false; ///< the most recent fetch failed
    TrackerConnectivityState Connectivity = TrackerConnectivityState::Unknown;
};

inline DataFreshness ClassifyFreshness(const FreshnessInputs& in) {
    if (!in.HasCache) {
        return in.InFlight ? DataFreshness::LoadingNoCache : DataFreshness::UnavailableNoCache;
    }
    if (in.InFlight) {
        return DataFreshness::Refreshing;
    }
    if (in.Live && !in.LastAttemptFailed) {
        return DataFreshness::Fresh;
    }
    return IsOfflineState(in.Connectivity) ? DataFreshness::CachedOffline : DataFreshness::CachedStale;
}

/// False only when there is nothing to show; every other state renders the (cached) content.
inline bool ShouldRenderContent(DataFreshness f) {
    return f != DataFreshness::LoadingNoCache && f != DataFreshness::UnavailableNoCache;
}

enum class WriteRoute : std::uint8_t { NetworkFirst, QueueImmediately, Reject };

/// `readOnlyPreference` is the user's Preferences switch — never the connectivity banner.
inline WriteRoute RouteWrite(TrackerConnectivityState s, bool queueSupported, bool readOnlyPreference) {
    if (readOnlyPreference) {
        return WriteRoute::Reject;
    }
    if (IsOfflineState(s) && queueSupported) {
        return WriteRoute::QueueImmediately;
    }
    return WriteRoute::NetworkFirst;
}

} // namespace offline
} // namespace smatchet
```

**Step 2 — `Source/Core/include/ScopeExit.h`.**
- Create the file with `#pragma once`, then this comment: `// ScopeExit — runs a callable when the
  scope ends, on every exit path including a throw. Used to clear in-flight latches so a failure can
  never leave a loader stuck (Quality Pillar 6). The callable must not throw.`
- Then `#include <functional>` and `#include <utility>`.
- Then, in `namespace smatchet { … }`, the class body copied **verbatim** from `class ScopeExit {` …
  `};` in `Source/Core/src/Sync/OfflineQueueService.cpp` (≈ lines 41-54).
- In `OfflineQueueService.cpp`: delete that class (`class ScopeExit {` through its `};`). Put
  `using smatchet::ScopeExit;` on the exact line where the class was, still inside the anonymous
  namespace. Add `#include "ScopeExit.h"` with the other quote includes.

**Step 3 — `Source/Core/include/KeyedLookupCache.h`:**
```cpp
#pragma once

// KeyedLookupCache — the one offline-aware state machine for a keyed, network-backed lookup (Quality
// Pillar 6). It replaces hand-rolled inFlight / loaded / retryAfter trios.
//
// Rules it enforces: a failed fetch never marks a key loaded and never discards a value that was
// already there; it records a retry-after deadline instead. No fetch starts while the tracker is
// offline or inside the backoff window. RunKeyedFetch clears InFlight on every exit path, including a
// throw. The mutex is never held while a fetch runs. Thread-safe.

#include "OfflineFirstPure.h"
#include "ScopeExit.h"
#include "Tracker/TrackerError.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace smatchet {
namespace offline {

template <typename Value> class KeyedLookupCache {
  public:
    struct Entry {
        bool HasValue = false;
        bool Live = false;
        bool InFlight = false;
        bool LastAttemptFailed = false;
        TrackerErrorKind LastErrorKind = TrackerErrorKind::None;
        std::string LastError;
        Value Payload{};
    };
    struct Ticket {
        std::string Key;
        std::uint64_t Gen = 0;
    };

    explicit KeyedLookupCache(int retryAfterSeconds = kLookupRetryAfterSeconds) : retryAfterSeconds_(retryAfterSeconds) {}

    Entry Get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(key);
        return it == slots_.end() ? Entry() : it->second.E;
    }

    DataFreshness Freshness(const std::string& key, TrackerConnectivityState connectivity) const {
        const Entry e = Get(key);
        FreshnessInputs in;
        in.HasCache = e.HasValue;
        in.Live = e.Live;
        in.InFlight = e.InFlight;
        in.LastAttemptFailed = e.LastAttemptFailed;
        in.Connectivity = connectivity;
        return ClassifyFreshness(in);
    }

    /// True (and `out` filled) when the caller should launch a fetch for `key` now.
    bool TryBeginFetch(const std::string& key, TrackerConnectivityState connectivity, Clock::time_point now,
                       Ticket& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& s = slots_[key];
        if (s.E.InFlight || (s.E.Live && !s.E.LastAttemptFailed)) {
            return false;
        }
        if (!ShouldAttemptNetwork(connectivity, now, s.RetryAfter)) {
            return false;
        }
        s.E.InFlight = true;
        s.Gen = ++nextGen_;
        out.Key = key;
        out.Gen = s.Gen;
        return true;
    }

    void CompleteSuccess(const Ticket& t, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(t.Key);
        if (it == slots_.end() || it->second.Gen != t.Gen) {
            return; // invalidated while in flight — drop the stale result
        }
        Entry& e = it->second.E;
        e.Payload = std::move(value);
        e.HasValue = true;
        e.Live = true;
        e.InFlight = false;
        e.LastAttemptFailed = false;
        e.LastErrorKind = TrackerErrorKind::None;
        e.LastError.clear();
        it->second.RetryAfter = Clock::time_point();
    }

    void CompleteFailure(const Ticket& t, const TrackerError& error, Clock::time_point now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(t.Key);
        if (it == slots_.end() || it->second.Gen != t.Gen) {
            return;
        }
        Entry& e = it->second.E;
        e.InFlight = false;
        e.LastAttemptFailed = true;
        e.LastErrorKind = error.Kind;
        e.LastError = error.Detail;
        it->second.RetryAfter = now + std::chrono::seconds(retryAfterSeconds_);
    }

    /// Seed a value restored from local storage. Never overrides a live value.
    void SeedFromStore(const std::string& key, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry& e = slots_[key].E;
        if (e.Live) {
            return;
        }
        e.Payload = std::move(value);
        e.HasValue = true;
    }

    /// Forget one key, e.g. after a successful write changed what the lookup returns.
    void Invalidate(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.erase(key);
    }

    /// Connectivity came back: clear every backoff so failed keys retry on their next use.
    void OnConnectivityRecovered() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : slots_) {
            kv.second.RetryAfter = Clock::time_point();
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.clear();
    }

  private:
    struct Slot {
        Entry E;
        Clock::time_point RetryAfter{};
        std::uint64_t Gen = 0;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Slot> slots_;
    std::uint64_t nextGen_ = 0;
    int retryAfterSeconds_;
};

/// Run `fetch` (returns Result<Value, TrackerError>) for a ticket from TryBeginFetch and record the
/// outcome. Call it on a worker thread. A throw is recorded as an Unknown failure (and rethrown), so
/// InFlight can never stay latched.
template <typename Value, typename FetchFn>
void RunKeyedFetch(KeyedLookupCache<Value>& cache, const typename KeyedLookupCache<Value>::Ticket& ticket,
                   FetchFn&& fetch) {
    bool recorded = false;
    ScopeExit recordThrow([&cache, &ticket, &recorded]() {
        if (!recorded) {
            cache.CompleteFailure(ticket, TrackerErrorUnknown("lookup fetch threw"), Clock::now());
        }
    });
    auto result = fetch();
    recorded = true;
    if (result.has_value()) {
        cache.CompleteSuccess(ticket, std::move(result.value()));
    } else {
        cache.CompleteFailure(ticket, result.error(), Clock::now());
    }
}

} // namespace offline
} // namespace smatchet
```

**Step 4 — freshness cue.**

`Source/Core/include/DataFreshnessCue.h`:
```cpp
#pragma once

// DataFreshnessCue — the one visible cue for data that is cached, refreshing or unavailable (Quality
// Pillar 6). Every network-backed view that can show cached data draws this instead of a hand-written
// "Loading..." line. Localized; draws nothing for DataFreshness::Fresh. UI thread only.

#include "OfflineFirstPure.h"

namespace DataFreshnessCue {

/// Localized short text for the state ("" for Fresh).
const char* CueText(smatchet::offline::DataFreshness f);
/// One disabled-text line; `detail` (e.g. the last error) shows as a hover tooltip when non-null and non-empty.
void Draw(smatchet::offline::DataFreshness f, const char* detail = nullptr);
/// Compact "(cached)" / "(refreshing)" badge for combo rows and grid cells; nothing for Fresh.
void DrawInlineBadge(smatchet::offline::DataFreshness f, const char* detail = nullptr);

} // namespace DataFreshnessCue
```

`Source/Core/src/DataFreshnessCue.cpp`:
- Include `"DataFreshnessCue.h"`, `"SmatchetLocalization.h"` and `"imgui.h"`.
- `CueText` is a `switch` returning `SmatchetLocalization::T(<key>, <English>)` for each state, using
  the keys below. `Fresh` returns `""`. `CachedStale` returns the `freshness.cached_stale` text.
- `Draw` returns if `CueText(f)[0] == '\0'`. Otherwise it calls `ImGui::TextDisabled("%s", CueText(f))`,
  then, if `detail && detail[0] && ImGui::IsItemHovered()`, calls `ImGui::SetTooltip("%s", detail)`.
- `DrawInlineBadge` draws the text of `freshness.badge_refreshing` for `Refreshing` and
  `LoadingNoCache`, `freshness.badge_cached` for `CachedOffline` and `CachedStale`, and nothing
  otherwise. It uses the same tooltip rule.

`SmatchetLocalization.cpp`:
- First run `grep -n '"freshness\.' Source/Core/src/SmatchetLocalization.cpp`; it must be empty.
- Insert these 7 entries on the line after
  `    {"comments.fetch_failed", "Failed to load comments.", u8"Impossible de charger les commentaires."},`:
```cpp
    {"freshness.refreshing", "Refreshing\xE2\x80\xA6", u8"Actualisation…"},
    {"freshness.cached_offline", "Offline \xE2\x80\x94 showing saved data", u8"Hors ligne — données enregistrées affichées"},
    {"freshness.cached_stale", "Showing saved data \xE2\x80\x94 the last refresh failed", u8"Données enregistrées affichées — la dernière actualisation a échoué"},
    {"freshness.loading_no_cache", "Loading\xE2\x80\xA6", u8"Chargement…"},
    {"freshness.unavailable_offline", "Not available offline yet", u8"Pas encore disponible hors ligne"},
    {"freshness.badge_cached", "(saved)", u8"(enregistré)"},
    {"freshness.badge_refreshing", "(refreshing)", u8"(actualisation)"},
```

**Step 5 — connectivity.**
- `ConnectivityMonitorService.h`:
  - Change the member `TrackerConnectivityState lastState_ = TrackerConnectivityState::Unknown;` to
    `std::atomic<TrackerConnectivityState> lastState_{TrackerConnectivityState::Unknown};`.
  - Change `GetLastState()` to `return lastState_.load();` and `SetLastState` to
    `lastState_.store(state);`.
  - Add a public method: `/// UI thread only: probe on the next tick instead of waiting out the interval
    (a live request just failed at the transport level, or a test brought the network back).` followed
    by `void RequestProbeNow() { nextProbeAt_ = std::chrono::steady_clock::now(); }`. It has no effect
    while a probe is already in flight; that is intended.
  - Update the header's THREADING comment by adding the sentence: "lastState_ is atomic because
    workers and the command/MCP threads read it."
  - Compile. Any `.cpp` line assigning or reading `lastState_` directly keeps working through the
    atomic's implicit load/store. Do not change them.
- `AppController.h`: next to the `GetLastTrackerConnectivityState` declaration, add
  `void RequestTrackerProbeNow();`.
- `AppController_Connectivity.cpp`: add
  `void AppController::RequestTrackerProbeNow() { if (connectivity_) { connectivity_->RequestProbeNow(); } }`,
  formatted like its neighbours.
- `Interfaces/IAppSync.h`:
  - Add `#include "OfflineFirstPure.h" // IsOfflineState (rank-0)`.
  - After `GetFieldCatalogError`, add:
    `/// Pillar 6: true while the last probe says the tracker is unreachable. Non-virtual on purpose.`
    followed by `bool IsTrackerOffline() const { return smatchet::offline::IsOfflineState(GetLastTrackerConnectivityState()); }`.

**Step 6 — deps.**
- In both `IEditMetaDeps.h` and `IFieldEditDeps.h`:
  - Add `#include "Types/ConnectivityTypes.h"`.
  - Add the pure virtual
    `/// Last connectivity probe result (Pillar 6 offline gating). Safe on any thread.` followed by
    `virtual TrackerConnectivityState TrackerConnectivity() const = 0;`.
- `GridContextDepsAdapter.h`: add ONE declaration, `TrackerConnectivityState TrackerConnectivity() const override;`.
  It overrides both interfaces.
- `GridContextDepsAdapter.cpp`: add
  `TrackerConnectivityState GridContextDepsAdapter::TrackerConnectivity() const { return app_.GetLastTrackerConnectivityState(); }`.
- `tests/support/FakeEditMetaDeps.h` and `FakeFieldEditDeps.h`: add the public member
  `TrackerConnectivityState ConnectivityImpl = TrackerConnectivityState::AuthenticatedReachable;` and
  `TrackerConnectivityState TrackerConnectivity() const override { return ConnectivityImpl; }`.
- Run `grep -rn "public IEditMetaDeps\|public IFieldEditDeps" Source tests`. It must list only these
  three classes; if it lists any other, STOP.

**Step 7 — tests.**
- `tests/Core/OfflineFirstPure.test.cpp` (pure; both lists) covers:
  - `IsOfflineState` for all 5 states;
  - `ShouldAttemptNetwork`: offline blocks, backoff blocks, a past `retryAfter` allows;
  - every `ClassifyFreshness` branch (6 outcomes);
  - `ShouldRenderContent`;
  - `RouteWrite`: preference wins, offline and queueable queue, offline and not queueable goes network,
    online goes network.
- `tests/Core/KeyedLookupCache.test.cpp` (pure; both lists) covers:
  1. The first `TryBeginFetch` returns true and a second returns false while in flight.
  2. `CompleteSuccess` gives `Fresh`, and a later `TryBeginFetch` returns false.
  3. `SeedFromStore(key, v)`, then `TryBeginFetch` (true), then `CompleteFailure` → `Get(key).Payload == v`,
     `HasValue` and `LastAttemptFailed` are true, and `Live` is false. A second `TryBeginFetch(key, …, now)`
     is false; at `now + std::chrono::seconds(31)` it is true.
  4. While `TransportDown`, `TryBeginFetch` returns false and `Freshness` is `UnavailableNoCache` or
     `CachedOffline`.
  5. `OnConnectivityRecovered` clears the backoff.
  6. `Invalidate` while in flight makes the stale `CompleteSuccess` a no-op.
  7. `RunKeyedFetch` with a lambda that throws `std::runtime_error`: `CHECK_THROWS(...)`, then
     `Get(key).InFlight == false` and `LastAttemptFailed == true`.
  8. `SeedFromStore` never overrides a live value.
  9. Concurrency: 8 `std::thread`s each call `TryBeginFetch` 1000× on the same key and at most one
     wins; join them all before asserting.

**Verify:** Linux `--test-case='OfflineFirst*,KeyedLookupCache*,ConnectivityMonitorService*,EditMetaCacheService*,FieldEditPipelineService*,OfflineQueue*'`.
**Done when:**
- gates are green;
- `grep -n "class ScopeExit" Source/Core/src` finds nothing;
- `DataFreshnessCue` compiles in `posix-core-check`. It is not used yet, which is fine.

---

## S3 — test(offline): fake-network harness + OfflineFirst bucket-E lane

**Symptom for the Issue:** "No test can take the tracker offline. The fixture backends cannot inject
network failures, so offline regressions ship unseen."

**Files**

| File | Change |
|---|---|
| `tests/support/FakeNetworkSwitch.h` | NEW |
| `tests/support/FakeTrackerClient.h` | network gate + new scriptable methods |
| `tests/support/JiraFakeTrackerFixture.h`, `.cpp` | new JSON keys |
| `tests/fixtures/jira_backend/offline-first.json` | NEW fixture |
| `tests/ui/offline_first.test.cpp` | NEW bucket-E group `OfflineFirst` |
| `tests/ui/ui_tests_registry.cpp`, `tests/ui/CMakeLists.txt` | register it |
| `scripts/dev/test-ui-offline-first.sh` | NEW driver wrapper |
| `.github/workflows/build-and-test.yml` | one new step in job `bucket-e-jira-fixture-mesa-gl` |
| `tests/Core/JiraFakeTrackerFixture.test.cpp` | cases for the new keys |

### S3 steps

**Step 1 — `tests/support/FakeNetworkSwitch.h`:**
```cpp
#ifndef SMATCHET_TESTS_FAKE_NETWORK_SWITCH_H
#define SMATCHET_TESTS_FAKE_NETWORK_SWITCH_H

// FakeNetworkSwitch — process-wide "is the network up" switch for fake tracker backends (Quality
// Pillar 6 offline-first harness). FakeTrackerClient consults it before every network-shaped call, so
// bucket-A doctests and bucket-E UI tests can take the tracker offline and bring it back.
// GlobalFakeNetwork() is an inline function with a function-local static: ONE instance per executable,
// shared by every TU. A namespace-scope static would give each TU its own copy.

#include "Tracker/TrackerError.h"

#include <atomic>

namespace smatchet_tests {

enum class FakeNetworkMode { Up = 0, TransportDown = 1, ServiceUnavailable = 2 };

class FakeNetworkSwitch {
  public:
    void Set(FakeNetworkMode m) { mode_.store(static_cast<int>(m)); }
    FakeNetworkMode Mode() const { return static_cast<FakeNetworkMode>(mode_.load()); }
    bool IsDown() const { return Mode() != FakeNetworkMode::Up; }
    /// The error a real client returns for the current outage.
    TrackerError MakeError() const {
        return Mode() == FakeNetworkMode::ServiceUnavailable ? TrackerErrorServer("fake network: HTTP 503", 503)
                                                             : TrackerErrorTransport("fake network: connection refused", 0);
    }
    /// Count a network-shaped call made while down (tests assert "no network while offline").
    void NoteCallWhileDown() { callsWhileDown_.fetch_add(1); }
    int CallsWhileDown() const { return callsWhileDown_.load(); }
    void ResetCounters() { callsWhileDown_.store(0); }

  private:
    std::atomic<int> mode_{0};
    std::atomic<int> callsWhileDown_{0};
};

inline FakeNetworkSwitch& GlobalFakeNetwork() {
    static FakeNetworkSwitch s;
    return s;
}

/// RAII: restores the global switch to Up and clears its counters when the test scope ends.
class ScopedFakeNetworkReset {
  public:
    ScopedFakeNetworkReset() { GlobalFakeNetwork().ResetCounters(); }
    ~ScopedFakeNetworkReset() {
        GlobalFakeNetwork().Set(FakeNetworkMode::Up);
        GlobalFakeNetwork().ResetCounters();
    }
    ScopedFakeNetworkReset(const ScopedFakeNetworkReset&) = delete;
    ScopedFakeNetworkReset& operator=(const ScopedFakeNetworkReset&) = delete;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_NETWORK_SWITCH_H
```

**Step 2 — `FakeTrackerClient.h`.** Keep every existing behaviour when no network is attached.
- **Header setup:**
  - Add `#include "FakeNetworkSwitch.h"` and `#include <stdexcept>`.
  - Update the header comment with one line: "AttachNetwork(&GlobalFakeNetwork()) makes every
    network-shaped call fail like a real outage while the switch is down."
- **Public API:**
  - `void AttachNetwork(FakeNetworkSwitch* net) { network_ = net; }`
  - `void EnableCollaboration(bool on) { collaborationEnabled_ = on; }`
  - Change `Collaboration()` to `return collaborationEnabled_ ? this : nullptr;`.
- **Private helper:**
  ```cpp
  bool NetworkDown(bool countCall = true) const {
      if (network_ != nullptr && network_->IsDown()) {
          if (countCall) {
              network_->NoteCallWhileDown();
          }
          return true;
      }
      return false;
  }
  ```
- **Down-gate as the FIRST statement of each existing method:**
  - `ProbeReachability`:
    ```cpp
    if (NetworkDown(false)) {
        return TrackerReachabilityProbeResult{network_->Mode() == FakeNetworkMode::ServiceUnavailable ? TrackerReachabilityProbeKind::ServiceUnavailable : TrackerReachabilityProbeKind::TransportDown, "fake network down"};
    }
    ```
    Probes are expected while offline, so they are not counted.
  - `FetchIssues`: set `*outFullSyncCompleted=false`, `*outFetchError=e.Detail`,
    `*outFetchErrorStructured=e` and clear `*outWarning`, each only if its pointer is non-null; then
    return an empty vector.
  - `FetchIssuesForKeys` and `FetchChildrenOfKeys`: `return Result<...>::Err(network_->MakeError());`.
  - `UpdateIssueFields`, `UpdateField` and `AddIssueToSprint`: `return network_->MakeError();`.
  - `CreateIssue`, `AttachFilesToIssue`, `FetchIssueEditMeta` and `FetchProjectComponents`:
    `return Result<...>::Err(network_->MakeError());`.
  - Each gate sits **before** the call is recorded, so a down call never appears in `...Calls()`.
- **New overrides**, each gated the same way:
  - `FetchFieldCatalog(const TrackerConfig&, const std::string&)`:
    - Returns `Err(TrackerErrorInvalidRequest("FetchFieldCatalog is not supported by this backend."))`
      unless `SetFieldCatalogResult(TrackerFieldCatalogResult)` was called. That keeps the default
      unchanged.
    - Counts calls in `fetchFieldCatalogCalls_`, exposed as `FetchFieldCatalogCalls()`.
  - `FetchIssueTransitions(const TrackerConfig&, const std::string& issueKeyOrId)`:
    - Uses a map set by `SetIssueTransitions(const std::string& issueId, std::vector<TrackerFieldOption>)`.
    - Otherwise returns the default error set by `SetIssueTransitionsError(TrackerError)`, or
      `Err(TrackerErrorInvalidRequest("FetchIssueTransitions not scripted"))`.
    - `SetIssueTransitionsThrows(bool)` makes it `throw std::runtime_error("scripted transitions throw")`
      after the network gate.
    - Counts calls as `FetchIssueTransitionsCalls()`.
  - `FetchIssueComments(const std::string& issueKey)`:
    - Uses a map set by `SetIssueComments(const std::string&, std::vector<TrackerIssueComment>)`; an
      unscripted key returns `Ok({})`.
    - Counts calls as `FetchIssueCommentsCalls()`.
  - `AddIssueCommentPlain(const TrackerConfig&, const std::string& issueKey, const std::string& plainText)`:
    - Records `{issueKey, plainText}` in `addCommentCalls_` (struct `AddCommentCall { std::string IssueKey; std::string Body; }`).
    - Returns the next scripted reply from `std::deque<TrackerError> addCommentReplies_`
      (`EnqueueAddCommentResult(TrackerError)`), default `TrackerError::Ok()`.
  - `AddWorklog(const TrackerConfig&, const std::string& issueKey, const std::string&, const std::string&, const std::string&, const std::string&, const std::string&)`:
    counts `addWorklogCalls_` and returns `TrackerError::Ok()`.
  - `AddIssueWatcher(const TrackerConfig&, const std::string& issueKey)`: counts `addWatcherCalls_` and
    returns `TrackerError::Ok()`.
- **Reset and state:**
  - Extend `ResetCalls()` to also zero every new counter and call list.
  - New private members: `FakeNetworkSwitch* network_ = nullptr; bool collaborationEnabled_ = false;`
    plus the maps, queues and counters above.

**Step 3 — `JiraFakeTrackerFixture`.** Parse four new optional top-level keys in `ParseJson`, store
them in new private members, and apply them in `Configure`.
- **In `Configure`, always first:** `client.AttachNetwork(&GlobalFakeNetwork());`.
- **`"network": {"mode": "Up"|"TransportDown"|"ServiceUnavailable"}`:**
  `GlobalFakeNetwork().Set(<mode>)` in `Configure`, but only when the key is present.
- **`"fieldCatalog": {"fields": [ {"id","name","family","options":[{"id","value"}]} ]}`:**
  - Build a `TrackerField` per entry: `Id`, `Name`, `Type = "option"`,
    `Family = ParseFamily(family)`, `AllowedValueOptions` from options, and `AllowedValues` from each
    option's value.
  - `ParseFamily` maps `"Status"`, `"Text"`, `"SelectSingle"`, `"SelectMulti"`, `"UserSingle"` and
    `"Labels"` to the enum values of the same name; anything else maps to `Unknown`.
  - In `Configure`: `client.SetFieldCatalogResult(result)` with `result.Fields` set.
- **`"transitions": {"KEY-1": [{"id":"3","name":"Done"}]}`:** `SetIssueTransitions` per key
  (`Id=id`, `Value=name`).
- **`"comments": {"KEY-1": [{"id","author","body","createdAtSec"}]}`:** `EnableCollaboration(true)`,
  then `SetIssueComments` per key.

**Step 4 — `tests/fixtures/jira_backend/offline-first.json`:**
- Copy `basic-grid.json` and keep its `reachability` and `fetches`.
- Make sure its tickets include `OFF-1` with `status` = `"To Do"` and `issuetype` = `"Bug"`, and a
  second ticket `OFF-2`.
- Add these keys:
  - `"fieldCatalog"`: a `status` field (family `Status`, options `1`/`To Do`, `2`/`In Progress`,
    `3`/`Done`, `4`/`Blocked`) and a `summary` field (family `Text`).
  - `"transitions"`: `{"OFF-1": [{"id":"2","name":"In Progress"},{"id":"3","name":"Done"}]}`.
  - `"comments"`: `{"OFF-1": [{"id":"c1","author":"Ana Offline","body":"cached comment body","createdAtSec":1700000000}]}`.
- If `basic-grid.json` uses `jiraSearchPages` rather than `cachedTickets`, add the tickets in that
  shape and set `fields.comment` so the comment blob maps. Then run
  `python -c "import json;json.load(open('tests/fixtures/jira_backend/offline-first.json'))"`.

**Step 5 — `tests/ui/offline_first.test.cpp`.** Mirror the structure and includes of
`tests/ui/jira_deterministic_backend.test.cpp` exactly, adding
`#include "FakeNetworkSwitch.h"` and `#include "Config/ConfigManager.h"`. Add:
- a helper `bool OfflineFirstFixtureActive(ImGuiTestContext* ctx)`: true when `std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE")`
  is set and contains `"offline-first"`; otherwise `ctx->LogInfo("SKIP: offline-first fixture not active"); return false;`;
- test `IM_REGISTER_TEST(engine, "OfflineFirst", "Catalog_SurvivesTransportDown")`:
  1. `smatchet_tests::ScopedFakeNetworkReset reset;`
  2. Get `AppController* app = SmatchetActiveUiTestAppController();`.
  3. `app->RefreshFieldCatalog(ConfigManager::Load());` while Up, then
     `IM_CHECK_NO_RET(!app->GetAvailableFields().empty());`.
  4. `smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);`
  5. `app->RefreshFieldCatalog(ConfigManager::Load());`
  6. `IM_CHECK_NO_RET(!app->GetAvailableFields().empty());` and
     `IM_CHECK_NO_RET(app->GetFieldCatalogError().empty());`.
  7. Check `app->GetTrackerConnectivityBannerForUi(nullptr).Kind != TrackerConnectivityBannerForUi::Level::Error`.
- `extern "C" void SmatchetRegisterOfflineFirstTests(ImGuiTestEngine* engine)` calling the register
  function. Later slices add more tests to this file and to this entry point.

Registration:
- `ui_tests_registry.cpp`: add the `extern "C"` declaration after the line
  `extern "C" void SmatchetRegisterOfflineConflictModalPanesTests(ImGuiTestEngine* engine);`.
- Also add the call after `    SmatchetRegisterOfflineConflictModalPanesTests(engine);`.
- `tests/ui/CMakeLists.txt`: add `"${CMAKE_CURRENT_SOURCE_DIR}/offline_first.test.cpp"` after the
  `offline_conflict_modal_panes.test.cpp` line in `_SMATCHET_UI_TEST_SOURCES`.

**Step 6 — `scripts/dev/test-ui-offline-first.sh`** (then `chmod +x`):
```bash
#!/usr/bin/env bash
# test-ui-offline-first.sh — bucket-E driver for the Quality Pillar 6 (offline-first) UI tests.
# Thin wrapper over test-ui-jira-deterministic-backend.sh: same exe, isolated user data and JSON
# parsing, with the offline-first fixture and the OfflineFirst test group. Exit codes are the wrapped
# driver's (0 pass, 1 fail, 2 binary missing / UI tests not built).
set -euo pipefail
export UI_TEST_FILTER="${UI_TEST_FILTER:-OfflineFirst}"
export SMATCHET_TEST_JIRA_BACKEND_FIXTURE="${SMATCHET_OFFLINE_FIRST_FIXTURE:-tests/fixtures/jira_backend/offline-first.json}"
exec bash "$(dirname "$0")/test-ui-jira-deterministic-backend.sh"
```

**Step 7 — CI.** In `.github/workflows/build-and-test.yml`, job `bucket-e-jira-fixture-mesa-gl`, insert
a new step immediately before `      - name: Upload jira-fixture per-test log`.
- **Name:** `Run offline-first fixture-backend bucket-E (hard)`.
- **Copy:** the previous step's `shell` and `env`, except drop `SMATCHET_TEST_JIRA_BACKEND_FIXTURE`
  and set `SMATCHET_UI_TEST_OUTLOG: ${{ github.workspace }}/build/tmp/offline-first-outlog.txt`.
- **`run:` body:** the previous step's body verbatim, with these changes:
  - replace `bash scripts/dev/test-ui-jira-deterministic-backend.sh` with
    `bash scripts/dev/test-ui-offline-first.sh`;
  - change the `jira-fixture:` log prefixes to `offline-first:`;
  - use the error text `offline-first bucket-E FAILED after ${attempts} attempt(s).`.

**Step 8 — `tests/Core/JiraFakeTrackerFixture.test.cpp`.** Add cases:
1. A fixture with `"network":{"mode":"TransportDown"}` makes `CreateClient()->FetchIssueTransitions(...)`
   return a `Transport` error, and `GlobalFakeNetwork().CallsWhileDown() == 1`. Use
   `ScopedFakeNetworkReset` in every new case.
2. `"transitions"` round-trips.
3. `"fieldCatalog"` builds a Status-family field with 4 options.
4. `"comments"` enables `Collaboration()` and round-trips.
5. No `"network"` key leaves the switch Up.

**Verify:** `posix-core-check` build, then Linux `--test-case='JiraFakeTrackerFixture*'`. That test is
Windows-only if it is not in the TSan list — then rely on CI. CI must show the new "offline-first"
step green.

**Done when:** CI job `bucket-e-jira-fixture-mesa-gl` shows both steps green, and
`Catalog_SurvivesTransportDown` passes (it depends on S1).

---

## S4 — docs(gates): Quality Pillar 6 offline-first — ADR-0026, lint gates, review + agent rules

No product code. The gates must pass on the current tree: delta-gated, with existing hits grandfathered.

**Files**

| File | Change |
|---|---|
| `docs/adr/0026-offline-first-quality-pillar.md` | NEW |
| `docs/agent-rules/quality-pillars.md` | § 6 + ownership row + intro |
| `AGENTS.md` | pillar row + contract-card row, net −1 line |
| `docs/agent-rules/cpp-rules.md` | § Tiered enforcement paragraph |
| `agents/scripts/project/lint-rules.d/72-offline-exact.sh`, `74-offline-heuristic.sh` | NEW |
| `agents/scripts/project/lint-rules.d/00-common.sh` | two rule-id arrays |
| `agents/scripts/project/test-lint-rules.sh` | loader, `--scan-offline`, selftest, `--diff` blocks |
| `tests/bats/lint_rules.bats` | new section |
| `agents/core/code-review.md` | Offline-first block + Sync quick-map text |
| `agents/project/offline-sync.md` | v3 |
| `docs/agent-rules/delegation.md` | offline-sync row |
| `Source/Core/src/Ui/AGENTS.md`, `Source/Core/src/Tracker/AGENTS.md` | leaf invariants |
| `docs/self-improvement/recurring-finding-classes.md`, `docs/self-improvement/postmortems.md` | ledgers |
| `docs/self-improvement/categories/debt/2026-09-24-*.md` (3 files) | out-of-scope finds |

### S4 steps

**Step 1 — `docs/adr/0026-offline-first-quality-pillar.md`.** Use the ADR-0015 format: a title line,
`**Status:** accepted (2026-09-24)`, a lead paragraph, then `## Considered options` and
`## Consequences`. Content:
- **Lead.** Offline-first becomes UX Quality Pillar 6, enforced like DRY. Evidence:
  - PR #2234's status combo blocked on a live fetch;
  - the comments modal ignored cached comments;
  - Jira catalog failures collapsed to `Unknown` (the #21b TODO) and wiped the catalog;
  - ~20 more sites (list the slice table's features).
- **Decision:**
  1. Four invariants (copy them from Step 2).
  2. The shared primitives: `OfflineFirstPure.h`, `KeyedLookupCache.h`, `DataFreshnessCue`.
  3. Additive SQLite `lookup_cache` (S5) and `pending_actions` (S8) tables for offline reads and writes.
  4. Two exact rules **block** now; five heuristics are **WARN-first**.
  5. The fake-network harness and the `OfflineFirst` bucket-E lane.
- **Considered options:**
  - aspirational only (rejected: #2234 shipped past review);
  - block every rule now (rejected: heuristic false positives on ImGui code);
  - a JSON snapshot per lookup kind (rejected: per-row learning from many workers needs atomic upserts,
    which SQLite already gives).
- **Graduation, per WARN rule, independent of the others:**
  - whole-tree `--scan-offline` hits burned to 0 (fixed or deviation-marked);
  - fewer than 10% false positives over ~20 PRs touching scope, tallied by `code-review` in
    `docs/high-integrity/offline-calibration.md` (created by the first PR that records a tally);
  - or a maintainer decision.
  - `offline-write-bypasses-queue` becomes absolute-0 at the end of S9.
- **Owners:** `offline-sync` (implementer), `code-review` (reviewer-of-record).

**Step 2 — `docs/agent-rules/quality-pillars.md`.**
- Change the intro sentence starting "Five north-star quality invariants" so it says six invariants:
  UX Pillars 1-4 and 6 (1-3 and 6 enforceable) and Engineering Pillar 5.
- Before `## Agent ownership`, insert:
```markdown
## 6. Offline-first — keep working offline, sync later (UX Pillar)

**Pillar 6** ([ADR-0026](../adr/0026-offline-first-quality-pillar.md)): with the tracker unreachable the app keeps showing what it last knew and keeps every change the user makes, then syncs when the connection returns. Offline, a tracker request spends up to ~90 s in its retry window, so anything that waits on it looks frozen.

**Enforceable invariants:**
1. A network-backed read that has cached data renders it with a freshness cue (`DataFreshnessCue`) — never a loading-only or empty state. "Loading…" alone is only for `DataFreshness::LoadingNoCache`.
2. A failed fetch never discards cached data and is never remembered as final: it backs off (`KeyedLookupCache`) and retries after reconnect; its in-flight flag clears on every path.
3. Every tracker write goes through the offline queue when the tracker is unreachable (`RouteWrite` → queue immediately) and replays on reconnect; toasts say what actually happened ("Queued offline" vs "Saved").
4. A `TrackerError` keeps its `Transport` kind wherever it is flattened; `TrackerErrorUnknown(<string>)` is never how a failure is reported.

**Gates**: `offline-write-bypasses-queue` and `tracker-error-kind-collapsed` block (delta-gated); `offline-loading-only-render`, `offline-inflight-latch-unguarded`, `offline-failure-cached-as-loaded`, `offline-cache-cleared` and `offline-network-read-ungated` warn first (mechanics: [`cpp-rules.md`](cpp-rules.md) § Tiered enforcement). **Tests**: bucket A with `GlobalFakeNetwork()` (`tests/support/FakeNetworkSwitch.h`); bucket E with `scripts/dev/test-ui-offline-first.sh`. **Tools**: `Source/Core/include/OfflineFirstPure.h`, `KeyedLookupCache.h`, `DataFreshnessCue.h`; whole-tree sweep `bash agents/scripts/project/test-lint-rules.sh --scan-offline`.
```
- Add an ownership row: `| 6. Offline-first | offline-sync (implementer), code-review (reviewer-of-record) | Two blocking offline gates + five WARN heuristics; the offline test lanes are the backstop. |`.

**Step 3 — `AGENTS.md`.** The file must end at 149 lines; check with `wc -l AGENTS.md`.

(a) Replace these two lines:
```
Five north-star invariants:
- **UX Pillars** 1-4 (user-facing; 1-3 enforceable / auto-fail PRs, 4 aspirational-backlogged) + **Engineering Pillar 5 — DRY** (blocking delta-gate, graduated from WARN-first 2026-06-21, [ADR-0015](docs/adr/0015-dry-quality-pillar-duplication-gate.md)):
```
with this one line:
```
Six north-star invariants — **UX Pillars** 1-4 + 6 (user-facing; 1-3 + 6 enforceable / auto-fail PRs, 4 aspirational-backlogged) + **Engineering Pillar 5 — DRY** (blocking delta-gate, graduated from WARN-first 2026-06-21, [ADR-0015](docs/adr/0015-dry-quality-pillar-duplication-gate.md)); Pillar 6 **Offline-first** per [ADR-0026](docs/adr/0026-offline-first-quality-pillar.md):
```
(b) Directly after the pillar-table row that starts `| 5 | Engineering | DRY |`, add:
```
| 6 | UX | Offline-first | Cached data renders with a freshness cue, never a loading-only state; a failed fetch never wipes cached data or is cached as final; offline writes persist to the queue and replay on reconnect; `TrackerError` keeps its Transport kind | `offline-sync` (implementer), `code-review` |
```
(c) Directly after the contract-card row that starts `| \`pr-numbered-temporal-comments\` (WARN)`, add:
```
| `offline-write-bypasses-queue`, `tracker-error-kind-collapsed` · WARN: `offline-loading-only-render`, `offline-inflight-latch-unguarded`, `offline-failure-cached-as-loaded`, `offline-cache-cleared`, `offline-network-read-ungated` | changed first-party C++ under `Source/` | Pillar 6 ([ADR-0026](docs/adr/0026-offline-first-quality-pillar.md)): the first two **blocking**, delta-gated per changed file (existing hits grandfathered); the five heuristics WARN-first → graduate per ADR-0026; whole-tree sweep `--scan-offline` |
```
(d) Fold the last paragraph, which starts `**Recommended companion — caveman**`, onto the end of the
paragraph that starts `Each agent declares a closed set of **capability tags**`, separated by one
space. Then delete the now-empty blank line and the old paragraph line.

Finally:
- Run `grep -rn "Five north-star" docs agents scripts tests`.
- Update prose matches in `docs/` to "Six north-star".
- If a script or test matches, STOP.

**Step 4 — `docs/agent-rules/cpp-rules.md`.** Insert after the paragraph that starts
`**Recurring-findings rules**` a new paragraph whose bold lead-in is "Offline-first rules (Quality Pillar 6, ADR-0026)", with ADR-0026 written as a markdown link to `../adr/0026-offline-first-quality-pillar.md` (relative to `docs/agent-rules/`).
It states:
- **Modules:** `lint-rules.d/72-offline-exact.sh` (blocking) and `74-offline-heuristic.sh` (WARN).
- **Delta semantics:** a changed file fails only when it has more hits of a blocking rule than its
  merge-base copy. A moved line does not fail; a same-file swap is not caught, which is accepted.
- **Scopes (from Step 5):** `offline-write-bypasses-queue` excludes `Tracker/`, `Sync/` and
  `FieldEditPipelineService.cpp`; `tracker-error-kind-collapsed` covers `Tracker/`, `include/Tracker/`
  and `include/ITracker*.h`, and allows the `classified.IsOk() ? TrackerErrorUnknown(x) : classified`
  fallback.
- **Escape:** one line directly above: `SMATCHET_DEVIATION(rule=<id>; reason=…; owner=…; revisit=…)`.
  For the WARN rules it may be within 3 lines above; for `offline-network-read-ungated`, anywhere in
  the file.
- **Sweep:** `--scan-offline`.

**Step 5 — `lint-rules.d/72-offline-exact.sh`:**
```bash
#!/usr/bin/env bash
# 72-offline-exact.sh — Quality Pillar 6 (offline-first) EXACT rules (sourced by test-lint-rules.sh, not
# run directly). BLOCKING, delta-gated per changed file: a file fails only when it has MORE hits of a
# rule than its merge-base copy, so existing hits are grandfathered. ADR-0026.
#
# offline-write-bypasses-queue — a tracker write (comment, worklog, watcher, field update, create,
# attach, sprint) called straight on the backend outside the queue seam. Offline, that write is lost;
# route it through the offline queue so it replays on reconnect. Exempt: Source/Core/src/Tracker/ (the
# clients), Source/Core/src/Sync/ (queue + replay), FieldEditPipelineService.cpp (commit-or-queue seam).
#
# tracker-error-kind-collapsed — TrackerErrorUnknown(<one variable>) in tracker code. It throws away the
# Transport kind, so an offline failure reads as permanent and callers wipe cached data (the #21b
# collapse behind the PR #2234 postmortem). Classify where the response is in hand. The
# `classified.IsOk() ? TrackerErrorUnknown(x) : classified` fallback is allowed (IsOk() on the line or
# the 2 lines above).
#
# Escape: // SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) on the nearest non-blank
# line above the hit.

OFFLINE_WRITE_RE='(Collaboration\(\)|Mutations\(\)|[A-Za-z_]*[Mm]utations[A-Za-z0-9_]*|[A-Za-z_]*[Cc]ollab[A-Za-z0-9_]*)[[:space:]]*(->|\.)[[:space:]]*(AddIssueCommentPlain|AddIssueCommentAnnotateContext|AddWorklog|AddIssueWatcher|UpdateIssueFields|UpdateField|CreateIssue|AttachFilesToIssue|AddIssueToSprint)[[:space:]]*\('
OFFLINE_KIND_COLLAPSE_RE='TrackerErrorUnknown\([[:space:]]*(std::move\([[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\)|[A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\)'

scan_offline_exact_file() {
    # $1 = file to read; $2 = logical repo path for scope + output (defaults to $1).
    local f="$1" logical="${2:-$1}"
    [ -f "$f" ] || return 0
    case "$logical" in Source/*.cpp|Source/*.h|Source/*.hpp) ;; *) return 0 ;; esac
    case "$logical" in */ThirdParty/*) return 0 ;; esac
    local write_scope=0 kind_scope=0
    case "$logical" in *.cpp) write_scope=1 ;; esac
    case "$logical" in
        Source/Core/src/Tracker/*|Source/Core/src/Sync/*|*/FieldEditPipelineService.cpp) write_scope=0 ;;
    esac
    case "$logical" in
        Source/Core/src/Tracker/*|Source/Core/include/Tracker/*|Source/Core/include/ITracker*.h) kind_scope=1 ;;
    esac
    [ "$write_scope" -eq 1 ] || [ "$kind_scope" -eq 1 ] || return 0
    local lineno=0 prev_dev_rule="" prev1="" prev2="" line s code
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno+1))
        if [[ "$line" =~ $DEV_RE ]]; then
            local body="${BASH_REMATCH[1]}" kv
            prev_dev_rule=""
            IFS=';' read -ra kvs <<< "$body"
            for kv in "${kvs[@]}"; do kv="${kv# }"; case "$kv" in rule=*) prev_dev_rule="${kv#rule=}" ;; esac; done
            prev2="$prev1"; prev1="$line"
            continue
        fi
        if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
        local suppress="$prev_dev_rule"; prev_dev_rule=""
        s="${line#"${line%%[![:space:]]*}"}"
        case "$s" in '//'*|'*'*|'/*'*) prev2="$prev1"; prev1="$line"; continue ;; esac
        code="${line%%//*}"
        if [ "$write_scope" -eq 1 ] && [ "$suppress" != "offline-write-bypasses-queue" ] \
            && [[ "$code" =~ $OFFLINE_WRITE_RE ]]; then
            printf 'offline-write-bypasses-queue\t%s:%s\n' "$logical" "$lineno"
        fi
        if [ "$kind_scope" -eq 1 ] && [ "$suppress" != "tracker-error-kind-collapsed" ] \
            && [[ "$code" =~ $OFFLINE_KIND_COLLAPSE_RE ]]; then
            case "$code$prev1$prev2" in
                *'IsOk()'*) ;;
                *) printf 'tracker-error-kind-collapsed\t%s:%s\n' "$logical" "$lineno" ;;
            esac
        fi
        prev2="$prev1"; prev1="$line"
    done < "$f"
}

compute_offline_exact_violations() {
    local f
    while IFS= read -r f; do [ -n "$f" ] && scan_offline_exact_file "$f"; done < <(list_first_party_cpp_files)
}

offline_delta_hits() {
    # $1 = scanner fn, $2 = merge-base, $3.. = rule ids. Scans each CHANGED first-party C++ file once at
    # HEAD and once at the merge-base, and prints the HEAD hits of every rule whose HEAD count exceeds the
    # merge-base count (a new file counts from zero, so a moved line never fails).
    local fn="$1" mb="$2"
    shift 2
    local changed f head_out base_out rule head_n base_n tmp
    changed="$(git diff --name-only --diff-filter=d "$mb" 2>/dev/null \
        | grep -E '^Source/.*\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true)"
    [ -n "$changed" ] || return 0
    tmp="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/offline_delta.$$")"
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        head_out="$("$fn" "$f" "$f")"
        [ -n "$head_out" ] || continue
        base_out=""
        if git show "$mb:$f" > "$tmp" 2>/dev/null; then
            base_out="$("$fn" "$tmp" "$f")"
        fi
        for rule in "$@"; do
            head_n="$(printf '%s\n' "$head_out" | grep -cF "${rule}"$'\t' || true)"
            [ "$head_n" -gt 0 ] || continue
            base_n="$(printf '%s\n' "$base_out" | grep -cF "${rule}"$'\t' || true)"
            if [ "$head_n" -gt "$base_n" ]; then
                printf '%s\n' "$head_out" | grep -F "${rule}"$'\t'
            fi
        done
    done <<< "$changed"
    rm -f "$tmp" 2>/dev/null || true
}
```

**Step 6 — `lint-rules.d/74-offline-heuristic.sh`:**
```bash
#!/usr/bin/env bash
# 74-offline-heuristic.sh — Quality Pillar 6 (offline-first) HEURISTIC rules (sourced by
# test-lint-rules.sh, not run directly). WARN-FIRST / ADVISORY: never touches $rc; diff-scoped to changed
# first-party .cpp files. Each graduates to blocking on its own per ADR-0026.
#
# offline-loading-only-render — a "Loading" line drawn in a TU that fetches, with no freshness cue within
# the window. Offline it stays up for the whole retry window (up to ~90 s) even when cached data exists.
# offline-inflight-latch-unguarded — an in-flight flag set true shortly before a launch/kick with no
# ScopeExit / RunKeyedFetch / TryBeginFetch / LaunchIntoSlot / catch nearby: a throw or a dropped task
# leaves the UI on "Loading" forever.
# offline-failure-cached-as-loaded — `loaded = true` next to a failure log with no retry/backoff token: an
# offline failure is remembered as final and never retried after reconnect.
# offline-cache-cleared — clearing catalog / user / component state; offline it cannot be fetched back
# (only backend-switch and pane-retirement resets are legitimate — mark those with a deviation).
# offline-network-read-ungated (file-level) — a backend Fetch/Search/List call outside Tracker/ and Sync/
# in a file that never consults connectivity.
#
# Escape: // SMATCHET_DEVIATION(rule=<id>; ...) within the 3 lines above the hit (anywhere in the file for
# offline-network-read-ungated).

OFFLINE_LOADING_RE='(TextDisabled|TextUnformatted|TextWrapped|Text|SetTooltip)[[:space:]]*\(.*("Loading|T\("[a-z0-9_.]*loading)'
OFFLINE_FETCHER_RE='LaunchBackgroundTask\(|std::async\(|Ensure[A-Z][A-Za-z]*Loaded\(|Kick[A-Z][A-Za-z]*\('
OFFLINE_CUE_RE='DataFreshnessCue|ClassifyFreshness|ShouldRenderContent'
OFFLINE_LATCH_RE='[A-Za-z_]*[Ii]n[Ff]light[A-Za-z_]*[[:space:]]*=[[:space:]]*true'
OFFLINE_LAUNCH_RE='std::async\(|Kick[A-Z][A-Za-z]*\(|Launch[A-Z][A-Za-z]*\('
OFFLINE_LATCH_GUARD_RE='ScopeExit|RunKeyedFetch|TryBeginFetch|LaunchIntoSlot|catch[[:space:]]*\('
OFFLINE_LOADED_TRUE_RE='[Ll]oaded[A-Za-z_]*[[:space:]]*=[[:space:]]*true'
OFFLINE_FAIL_LOG_RE='LOG_(WARN|ERROR)\(.*([Ff]ail|[Ee]rror)'
OFFLINE_BACKOFF_RE='[Rr]etry|CompleteFailure|[Bb]ackoff'
OFFLINE_CLEAR_RE='(AvailableFields|AvailableComponents|AvailableIssueTypeMeta|AvailableUsers|projectComponentOptions_)\.clear\(\)|SetAvailableUsers\([[:space:]]*\{\}[[:space:]]*\)'
OFFLINE_NET_READ_RE='(FieldCatalog\(\)|Collaboration\(\)|Reader\(\)|Connectivity\(\))[[:space:]]*(->|\.)[[:space:]]*(Fetch|Search|List)[A-Za-z]*\('
OFFLINE_NET_GATE_RE='ShouldAttemptNetwork|IsOfflineState|IsTrackerOffline|TryBeginFetch|RunKeyedFetch|RouteWrite|TrackerConnectivity\(\)'
OFFLINE_HEUR_WINDOW="${SMATCHET_OFFLINE_WINDOW:-25}"

_offline_dev_above() {
    # $1 = line index, $2 = rule id. True when a deviation for the rule sits within the 3 lines above.
    local i="$1" rule="$2" j
    for ((j = i - 1; j >= 0 && j >= i - 3; j--)); do
        case "${_OFF_LINES[$j]}" in *"SMATCHET_DEVIATION(rule=$rule"*) return 0 ;; esac
    done
    return 1
}

_offline_window_has() {
    # $1..$2 = inclusive index range (clamped), $3 = ERE. True when any line in range matches.
    local from="$1" to="$2" re="$3" j n=${#_OFF_LINES[@]}
    [ "$from" -lt 0 ] && from=0
    [ "$to" -ge "$n" ] && to=$((n - 1))
    for ((j = from; j <= to; j++)); do
        [[ "${_OFF_LINES[$j]}" =~ $re ]] && return 0
    done
    return 1
}

scan_offline_heuristic_file() {
    # $1 = file to read; $2 = logical repo path (defaults to $1).
    local f="$1" logical="${2:-$1}"
    [ -f "$f" ] || return 0
    case "$logical" in Source/*.cpp) ;; *) return 0 ;; esac
    case "$logical" in */ThirdParty/*) return 0 ;; esac
    _OFF_LINES=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do _OFF_LINES+=("$line"); done < "$f"
    local n=${#_OFF_LINES[@]} i raw s code w="$OFFLINE_HEUR_WINDOW" file_text
    file_text="$(cat "$f")"
    local file_fetches=0 file_gated=0 net_reported=0 in_domain=0
    [[ "$file_text" =~ $OFFLINE_FETCHER_RE ]] && file_fetches=1
    [[ "$file_text" =~ $OFFLINE_NET_GATE_RE ]] && file_gated=1
    case "$logical" in Source/Core/src/Tracker/*|Source/Core/src/Sync/*) in_domain=1 ;; esac
    for ((i = 0; i < n; i++)); do
        raw="${_OFF_LINES[$i]}"
        s="${raw#"${raw%%[![:space:]]*}"}"
        case "$s" in '//'*|'*'*|'/*'*) continue ;; esac
        code="${raw%%//*}"
        if [ "$file_fetches" -eq 1 ] && [[ "$code" =~ $OFFLINE_LOADING_RE ]] \
            && ! _offline_window_has $((i - w)) $((i + w)) "$OFFLINE_CUE_RE" \
            && ! _offline_dev_above "$i" offline-loading-only-render; then
            printf 'offline-loading-only-render\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [[ "$code" =~ $OFFLINE_LATCH_RE ]] \
            && _offline_window_has "$i" $((i + w)) "$OFFLINE_LAUNCH_RE" \
            && ! _offline_window_has $((i - 5)) $((i + 60)) "$OFFLINE_LATCH_GUARD_RE" \
            && ! _offline_dev_above "$i" offline-inflight-latch-unguarded; then
            printf 'offline-inflight-latch-unguarded\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [[ "$code" =~ $OFFLINE_LOADED_TRUE_RE ]] \
            && _offline_window_has $((i - 6)) $((i + 6)) "$OFFLINE_FAIL_LOG_RE" \
            && ! _offline_window_has $((i - 10)) $((i + 10)) "$OFFLINE_BACKOFF_RE" \
            && ! _offline_dev_above "$i" offline-failure-cached-as-loaded; then
            printf 'offline-failure-cached-as-loaded\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [[ "$code" =~ $OFFLINE_CLEAR_RE ]] && ! _offline_dev_above "$i" offline-cache-cleared; then
            printf 'offline-cache-cleared\t%s:%s\n' "$logical" "$((i + 1))"
        fi
        if [ "$net_reported" -eq 0 ] && [ "$in_domain" -eq 0 ] && [ "$file_gated" -eq 0 ] \
            && [[ "$code" =~ $OFFLINE_NET_READ_RE ]] \
            && [[ "$file_text" != *"SMATCHET_DEVIATION(rule=offline-network-read-ungated"* ]]; then
            printf 'offline-network-read-ungated\t%s:%s\n' "$logical" "$((i + 1))"
            net_reported=1
        fi
    done
}

compute_offline_heuristic_violations() {
    local f
    while IFS= read -r f; do [ -n "$f" ] && scan_offline_heuristic_file "$f"; done < <(list_first_party_cpp_files | grep -E '\.cpp$' || true)
}
```

**Step 7 — `00-common.sh`.** After the line `SLURP_RULES=(unbounded-file-slurp)`, add:
```bash

# Quality Pillar 6 offline-first (ADR-0026) — exact rules BLOCK (delta-gated per changed file); the
# heuristics are WARN-first. KEEP IN SYNC with AGENTS.md § Enforcement contract-card.
OFFLINE_EXACT_RULES=(offline-write-bypasses-queue tracker-error-kind-collapsed)
OFFLINE_WARN_RULES=(offline-loading-only-render offline-inflight-latch-unguarded offline-failure-cached-as-loaded offline-cache-cleared offline-network-read-ungated)
```

**Step 8 — `test-lint-rules.sh`.**

(a) **Header list.** In the "Rule ids" comment list, add two lines after the
`(advisory)             unbounded-file-slurp` line:
```
#   offline-write-bypasses-queue / tracker-error-kind-collapsed  Pillar 6 exact rules (blocking, delta per file)
#   (advisory)             offline-* heuristics (Pillar 6, WARN-first; 74-offline-heuristic.sh)
```

(b) **Loader.** Change `"$LINT_RULES_D"/70-ui-request-flag.sh \` into
`"$LINT_RULES_D"/70-ui-request-flag.sh "$LINT_RULES_D"/72-offline-exact.sh "$LINT_RULES_D"/74-offline-heuristic.sh \`
by inserting the two modules right after 70 on the same continued line. Keep the list's order.

(c) **Parser.** Add `    --scan-offline) MODE=scanoffline ;;` after `    --scan-tu-ceiling) MODE=scantuceiling ;;`,
and add `|--scan-offline` to the usage string right after `|--scan-tu-ceiling`.

(d) **Dispatch.** After the `scanprcomments)` case block (it ends `    ;;`), add:
```bash
  scanoffline)
    # Quality Pillar 6 offline-first — whole-tree sweep of all seven rules (campaign + calibration).
    # `--root <dir>` (handled above) points this at an arbitrary tree.
    compute_offline_exact_violations
    compute_offline_heuristic_violations
    ;;
```

(e) **Selftest.** Immediately before the selftest's python delegation (the block that runs each
`*_audit.py --selftest`), add a block modelled on the ui-request-flag block:
- For each id in `OFFLINE_EXACT_RULES` and `OFFLINE_WARN_RULES`, run
  `if ! grep -qF "$r" AGENTS.md; then echo "SELFTEST FAIL: offline rule '$r' missing from AGENTS.md" >&2; miss=1; fi`.
- Create a temp `.cpp` exactly as the ui-request-flag block does, then assert each of the following.
  Pass the logical path as the scanner's second argument.

| Case | Fixture content (printf) | Scanner / logical path | Expect |
|---|---|---|---|
| write fires | `void F(Backend& b) {\n    b.Collaboration()->AddWorklog(cfg, k, a, b2, c, d, e);\n}\n` | `scan_offline_exact_file "$tmp" Source/Core/src/Ui/X.cpp` | non-empty |
| write exempt in Sync | same | `... Source/Core/src/Sync/X.cpp` | empty |
| write deviation | `// SMATCHET_DEVIATION(rule=offline-write-bypasses-queue; reason=t; owner=x; revisit=2099-01-01)\n` + the same call line | `... Source/Core/src/Ui/X.cpp` | empty |
| kind fires | `return Err(TrackerErrorUnknown(std::move(outError)));\n` | `... Source/Core/src/Tracker/X.cpp` | non-empty |
| kind IsOk idiom | `return classified.IsOk() ? TrackerErrorUnknown(outError) : classified;\n` | same | empty |
| kind literal | `return TrackerErrorUnknown("fixed text");\n` | same | empty |
| loading fires | `void D() {\n    app.LaunchBackgroundTask([](){});\n    ImGui::TextDisabled("Loading things...");\n}\n` | `scan_offline_heuristic_file "$tmp" Source/Core/src/Ui/X.cpp` | contains `offline-loading-only-render` |
| loading with cue | the same plus the line `    DataFreshnessCue::Draw(f);` | same | no `offline-loading-only-render` |
| latch fires | `void K() {\n    s.FetchInFlight = true;\n    app.LaunchBackgroundTask([](){});\n}\n` | same | contains `offline-inflight-latch-unguarded` |
| latch guarded | the same plus the line `    ScopeExit g([](){});` | same | no `offline-inflight-latch-unguarded` |

- End with `rm -f "$tmp"`. Every failed expectation prints `SELFTEST FAIL: <case>` and sets `miss=1`.

(f) **`--diff` blocking block.** Insert directly after the ui-request-flag `--diff` block. Its last lines
are `        echo "[test-lint-rules] PASS — no off-UI-thread g_ui request-flag write in command-dispatch TUs"`
and `    fi`. Insert:
```bash

    # --- Quality Pillar 6 offline-first EXACT rules (changed files; BLOCKING, delta per file) ---
    # offline-write-bypasses-queue + tracker-error-kind-collapsed (72-offline-exact.sh; ADR-0026). A
    # changed file fails only when it has MORE hits than its merge-base copy (existing hits are
    # grandfathered). A SMATCHET_DEVIATION(rule=<id>; ...) on the line above escapes.
    ofx_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    ofx_out="$(offline_delta_hits scan_offline_exact_file "$ofx_mb" "${OFFLINE_EXACT_RULES[@]}" | grep -E . || true)"
    if [ -n "$ofx_out" ]; then
        rc=1
        echo
        echo "FAIL: Quality Pillar 6 (offline-first) — new offline-breaking code (ADR-0026):"
        printf '%s\n' "$ofx_out" | sed 's/^/  /'
        echo "  offline-write-bypasses-queue: route the write through the offline queue so it replays on reconnect."
        echo "  tracker-error-kind-collapsed: classify at the failure site (ClassifyRejectedHttpStatus / TrackerErrorFromHttpStatus / TrackerErrorParse)."
        echo "  Genuine exception: add // SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) above the line."
    else
        echo "[test-lint-rules] PASS — no new Pillar 6 offline-first exact-rule hit"
    fi
```

(g) **`--diff` WARN block.** Insert directly after the pr-numbered-temporal-comments WARN block, which
ends with `        } >&2` and `    fi`. Insert:
```bash

    # --- Quality Pillar 6 offline-first HEURISTICS (changed .cpp; WARN-first, never touches $rc) ---
    ofh_mb="$(git merge-base "$BASE" HEAD 2>/dev/null || echo "$BASE")"
    ofh_out="$(offline_delta_hits scan_offline_heuristic_file "$ofh_mb" "${OFFLINE_WARN_RULES[@]}" | grep -E . || true)"
    if [ -n "$ofh_out" ]; then
        {
            echo "[offline-first] WARN: possible offline-breaking pattern (Quality Pillar 6, ADR-0026). Advisory; not blocking:"
            printf '%s\n' "$ofh_out" | sed 's/^/  /'
            echo "  Render cached data + DataFreshnessCue, gate fetches with KeyedLookupCache / IsOfflineState, back off on failure."
            echo "  If intended, add // SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) above the line."
        } >&2
    fi
```

**Step 9 — `tests/bats/lint_rules.bats`.** Add a section
`# ---------- offline-first (Quality Pillar 6; ADR-0026) ----------` before the "missing module" test.
Its tests use the ui-reqflag test shape: temp repo, `git init`, `git add -A`, then
`run bash "$LINT" --root "$tmp" --scan-offline`.

| Test name | Fixture path | Assert |
|---|---|---|
| `--scan-offline flags a direct backend write outside the queue seam` | `Source/Core/src/Ui/X.cpp` with a `Collaboration()->AddWorklog(` call | status 0, output has `offline-write-bypasses-queue` |
| `--scan-offline exempts Sync/ and Tracker/ writes` | the same line in `Source/Core/src/Sync/Q.cpp` and `Source/Core/src/Tracker/C.cpp` | no `offline-write-bypasses-queue` |
| `--scan-offline flags a collapsed TrackerErrorUnknown in tracker code` | `Source/Core/src/Tracker/C.cpp` with `return Err(TrackerErrorUnknown(outError));` | output has `tracker-error-kind-collapsed` |
| `--scan-offline allows the IsOk() fallback idiom` | `...? TrackerErrorUnknown(outError) : classified;` preceded by `classified.IsOk()` on the same line | no hit |
| `--scan-offline ignores comment and string mentions` | `// Collaboration()->AddWorklog(` | empty |
| `--scan-offline respects SMATCHET_DEVIATION` | deviation line above a write | no `offline-write-bypasses-queue` |
| `--scan-offline flags a loading-only render` | the loading fixture from Step 8e | has `offline-loading-only-render` |

Also add this test:
```bash
@test "--diff fails on a NEW offline write and passes on a grandfathered one" {
    tmp="$(mktemp -d)"
    ( cd "$tmp" && git init -q && git config user.email t@t && git config user.name t ) >/dev/null
    mkdir -p "$tmp/Source/Core/src/Ui"
    printf 'void A(B& b) {\n    b.Collaboration()->AddWorklog(c, k, a, b2, c2, d, e);\n}\n' > "$tmp/Source/Core/src/Ui/Old.cpp"
    ( cd "$tmp" && git add -A && git commit -qm base && git branch develop ) >/dev/null
    printf '// moved\nvoid A(B& b) {\n    b.Collaboration()->AddWorklog(c, k, a, b2, c2, d, e);\n}\n' > "$tmp/Source/Core/src/Ui/Old.cpp"
    ( cd "$tmp" && git add -A && git commit -qm moved ) >/dev/null
    run bash -c "cd '$tmp' && source '$REPO_ROOT/agents/scripts/project/lint-rules.d/00-common.sh' && source '$REPO_ROOT/agents/scripts/project/lint-rules.d/72-offline-exact.sh' && offline_delta_hits scan_offline_exact_file develop offline-write-bypasses-queue"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    printf 'void N(B& b) {\n    b.Mutations()->UpdateIssueFields(k, f);\n}\n' > "$tmp/Source/Core/src/Ui/New.cpp"
    ( cd "$tmp" && git add -A && git commit -qm new ) >/dev/null
    run bash -c "cd '$tmp' && source '$REPO_ROOT/agents/scripts/project/lint-rules.d/00-common.sh' && source '$REPO_ROOT/agents/scripts/project/lint-rules.d/72-offline-exact.sh' && offline_delta_hits scan_offline_exact_file develop offline-write-bypasses-queue"
    [[ "$output" == *"New.cpp"* ]]
}
```
If sourcing `00-common.sh` alone errors because it expects variables the entry script sets, STOP and
report. Do not change `00-common.sh` beyond Step 7.

Finally, run `bash agents/scripts/project/test-lint-rules.sh --scan-offline | sort | uniq -c | sort -rn | head -50`
and paste the per-rule counts into the PR body (the calibration baseline).

**Step 10 — `agents/core/code-review.md`.**
- Directly before the line starting `**Subsystem invariants**`, add the block below. The file lands at
  ~191 lines, below the 250 cap.
- In the quick map, change the `Sync/` bullet to:
  `` - `Sync/` — every backend write through `OfflineQueueService`; replay reuses the live pipelines; offline writes queue first (Pillar 6). ``
```markdown
**Offline-first (UX Quality Pillar 6; ADR-0026)** — you are reviewer-of-record with `offline-sync`. The blocking gates (`offline-write-bypasses-queue`, `tracker-error-kind-collapsed`) stop the exact regressions; the five WARN heuristics are yours to confirm or dismiss. For each new or changed network-backed read or write, picture the tracker unreachable:
- Read: the user sees the cached value plus a `DataFreshnessCue`, never a loading-only or empty state; the fetch is skipped while offline and backs off after a failure (never remembered as loaded); its in-flight flag clears on every path (`KeyedLookupCache` + `RunKeyedFetch`). A loading-only render while a cache exists = **High**.
- Write: it reaches the offline queue so it replays on reconnect, and the toast states the real outcome ("Queued offline" vs "Saved"). A write lost offline = **High**.
- Errors: every place a `TrackerError` is flattened keeps the `Transport` kind. A collapse to `Unknown` = **High**.
- Test: an offline case exists — bucket A with `GlobalFakeNetwork()` or bucket E via `scripts/dev/test-ui-offline-first.sh`. Missing = **Medium**.
```

**Step 11 — `agents/project/offline-sync.md` → v3.**
- **Frontmatter:**
  - `version: 3`.
  - Replace `description:` with: `SQLite cache, offline-queue replay, audit trail AND offline-first reads (Quality Pillar 6) — LocalCacheManager, OfflineQueueService, SmatchetOfflineQueueUi, TicketSyncService, BackendAuditTrail, FieldEditAuditSource, KeyedLookupCache, lookup_cache, DataFreshnessCue. Use for cache schema additions, queue replay, dead-letter handling, sync diff resolution, audit entries, and any feature that must keep working with the tracker unreachable.`
  - Append these triggers to `triggers:`: `connectivity`, `transport`, `stale`, `cached`, `offline-first`, `freshness`.
- **Banner:** change both banner lines' `v2` to `v3`.
- **Hard invariants:** add three bullets:
  - `**Cache-first reads.** A network-backed view renders its cached value with a DataFreshnessCue; it never shows loading-only while a cache exists (see the "Offline-first reads" section of `Source/Core/src/Ui/AGENTS.md`).`
  - `**Never wipe on error.** A failed fetch keeps whatever was cached and backs off; transport kind is preserved (Tracker/AGENTS.md).`
  - `**Queue first offline.** A write made while the tracker is unreachable persists to the queue immediately and replays on reconnect.`
- **Workflow step 4:** replace it with: `Build (cmake --preset posix-core-check / ninja-test-linux on Linux; ninja-iter-msvc on Windows). Offline smoke: bucket A — wrap a test in smatchet_tests::ScopedFakeNetworkReset and set GlobalFakeNetwork() to TransportDown (tests/support/FakeNetworkSwitch.h); bucket E — bash scripts/dev/test-ui-offline-first.sh; manual — block the tracker host (hosts file / firewall) or disconnect, make the change, reconnect, confirm replay + audit entry. There is no in-app network toggle.`
- **`## Smoke-test result`:** change "Offline path smoke: network off → …" to reference the harness above.

**Step 12 — `docs/agent-rules/delegation.md`.** In the table row that starts `` | `offline-sync` | low · read-edit | ``,
replace the description cell with the new frontmatter description (the shortened first sentence is
fine).

**Step 13 — leaf docs.**
- `Source/Core/src/Ui/AGENTS.md`: insert before `## Steady-state perf (Pillar 1)`:
```markdown
## Offline-first reads (Pillar 6)

- **Never render a loading-only state while cached data exists.** A network-backed view (combo, modal, panel, cell) draws its cached value and a `DataFreshnessCue` (`Source/Core/include/DataFreshnessCue.h`); "Loading…" alone is only for `DataFreshness::LoadingNoCache`. Offline, a tracker request spends up to ~90 s in its retry window, so a loading-only branch looks frozen and hides data the user already has (the PR #2234 status combo and the comments modal both did this).
- **Keyed lookups go through `KeyedLookupCache` + `RunKeyedFetch`** (`Source/Core/include/KeyedLookupCache.h`): no fetch while `IsOfflineState`, a failure backs off and is never remembered as loaded, and the in-flight flag clears on every path including a throw.
- **A tracker error banner never discards the user's edits.** Only the Read-only preference may drop not-yet-sent grid edits; a banner-driven read-only state holds them until the tracker is usable.
```
- `Source/Core/src/Tracker/AGENTS.md`: append to `## Invariants`:
```markdown
- **Every failure exit keeps its error kind.** Classify where the response is in hand (`ClassifyRejectedHttpStatus`, `TrackerErrorFromHttpStatus`, `TrackerErrorParse`); status 0 is `Transport`. Never report a failure as `TrackerErrorUnknown(<flattened string>)` — callers use `IsRetryable()` to choose between "keep cached data and retry" and "show an error", and an offline failure mislabelled as permanent once wiped the whole field catalog (gate `tracker-error-kind-collapsed`).
```

**Step 14 — ledgers.**
- `docs/self-improvement/recurring-finding-classes.md`: before `## Reconciliation vs`, add
  `## Offline-first batch (ADR-0026, 2026-09)` with a table in the file's format.

| Rank | Class | Recurrence evidence | Gate shipped | Tier |
|---|---|---|---|---|
| 1 | Write bypasses the offline queue | comment post, worklog, watch, Annotate, command/Lua field edits, bulk import (offline-first sweep 2026-09-24) | `offline-write-bypasses-queue` | **blocking** (delta) |
| 2 | Error kind collapsed to Unknown | #21b TODO in `JiraClient::FetchFieldCatalog` wiped the catalog offline; Linear team lookup | `tracker-error-kind-collapsed` | **blocking** (delta) |
| 3 | Loading-only render while cache exists | PR #2234 status combo; comments modal; components editor | `offline-loading-only-render` | WARN-first |
| 4 | In-flight latch without an exit guard | comments modal; transitions service; plan-doc viewer (#2056/#2057/#2108) | `offline-inflight-latch-unguarded` | WARN-first |
| 5 | Failure cached as final / no backoff | transitions service; editmeta per-frame storm | `offline-failure-cached-as-loaded` | WARN-first |
| 6 | Cached state cleared on error | catalog + users wipe on catalog failure | `offline-cache-cleared` | WARN-first |
| 7 | Network read with no connectivity gate | comments modal, tooltip fetch, project picker | `offline-network-read-ungated` | WARN-first |

- `docs/self-improvement/postmortems.md`: insert this entry directly below
  `<!-- Latest first. Append new entries at the top. -->`, followed by a blank line:
```markdown
## 2026-09-24 · PR #2234 · design escape: the status combo stopped working offline (no gate existed)

### What escaped
PR #2234 made the status combo wait for a live `/transitions` fetch before showing any option ("Loading transitions…"). Offline that fetch spends 1–90 s in the tracker retry window per issue; a failure was cached as final and never reset on reconnect. It shipped on top of an older collapse (#21b TODO): every Jira field-catalog failure became `TrackerErrorUnknown`, so an offline refresh wiped the catalog, turned the grid read-only and dropped pending edits. The same sweep found the comments modal ignoring its cached thread and ~20 further network-only paths.

### Root cause
No invariant, gate or review item covered offline behaviour: reviews checked that network work was off the UI thread (Pillar 2) but not what the user sees when it fails. The fixture backends could not simulate an outage, so no test could observe it. `postmortem-owed.sh` had no signal to key on — every gate was green because none existed.

### Preventing gate
UX Quality Pillar 6 (ADR-0026): blocking `offline-write-bypasses-queue` + `tracker-error-kind-collapsed`, five WARN-first heuristics (`offline-loading-only-render`, `offline-inflight-latch-unguarded`, `offline-failure-cached-as-loaded`, `offline-cache-cleared`, `offline-network-read-ungated`), the fake-network harness (`tests/support/FakeNetworkSwitch.h`) and the `OfflineFirst` bucket-E lane, plus the code-review Offline-first block.

### Eval case
The PR #2234 diff of `TicketFieldEditor.cpp` + `IssueTransitionsCacheService.cpp`: a reviewer must flag the loading-only combo branch and the failure cached as loaded as High.

### Filed as
`docs/plans/offline-first.md` (slices S1–S13).
```

**Step 15 — debt entries.** Create these three files, each in the format of
`docs/self-improvement/categories/debt/2026-09-20-cell-editor-commit-closes-combo-mid-gesture.md`
(one `- date · source · [debt] · P2 — title` line, then Details, Concrete next action,
`Status: open`, `Last-reviewed: 2026-09-24`):
1. `2026-09-24-pending-counts-sqlite-select-per-frame.md`. `SmatchetUI.cpp` calls
   `GetPendingFieldEdits()` and the status bar calls `GetPendingCreateCount()`, and each runs a SQLite
   SELECT every frame on the UI thread (Pillar 2). Next action: keep counts in `OfflineQueueService`,
   updated after enqueue/replay on workers.
2. `2026-09-24-updateticket-savesticket-on-ui-thread.md`. `AppController::UpdateTicket` →
   `Cache->SaveTicket` runs on the UI thread (`AppController_CatalogAndFieldEdit.cpp`). Next action:
   post the save to a worker.
3. `2026-09-24-project-picker-reads-catalog-cache-file-per-frame.md`. `SmatchetProjectPicker.cpp`
   `DrawRecentSection` calls `FieldCatalogCache::ListCachedProjects()` (file read + JSON parse) every
   frame the combo is open. Next action: cache the list while the popup is open.

**Verify:**
- `bash agents/scripts/project/test-lint-rules.sh --selftest` must pass.
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` must PASS; the offline WARN
  lines are allowed.
- Run the bats suite (contract §9).
- `bash agents/scripts/project/test-subsystem-docs.sh --diff origin/develop`
- `python3 agents/scripts/core/agent_size_audit.py --diff origin/develop`
- `wc -l AGENTS.md` must print 149.

**Done when:** all of those pass and the PR body carries the `--scan-offline` counts.

---

## S5 — fix(status): status combo never blocks offline; remember valid transitions `[visual]`

**Symptom for the Issue:** "Status dropdown shows only 'Loading transitions…' for up to 90 s per issue
offline; a failed fetch is remembered until restart (regression from PR #2234)."

**Files**

| File | Change |
|---|---|
| `Source/Core/include/Persistence/ILookupCache.h` | NEW |
| `Source/Core/include/Persistence/LocalCacheManager.h`, `Source/Core/src/Persistence/LocalCacheManager.cpp` | implement ILookupCache; call new schema init |
| `Source/Core/src/Persistence/LocalCacheManager_Lookup.cpp` | NEW companion TU |
| `Source/Core/include/IEditMetaDeps.h`, `GridContextDepsAdapter.{h,cpp}`, `tests/support/FakeEditMetaDeps.h` | `LookupCacheShared()` + `CacheBackendKey()` |
| `tests/support/FakeLookupCache.h` | NEW |
| `Source/Core/include/Types/TransitionsTypes.h` | NEW (query + lookup structs) |
| `Source/Core/include/LearnedWorkflowPure.h`, `Source/Core/src/LearnedWorkflowPure.cpp` | NEW |
| `Source/Core/include/StatusComboOptionsPure.h` | NEW |
| `Source/Core/include/IssueTransitionsCacheService.h`, `.cpp` | rewrite on KeyedLookupCache + learned store |
| `Source/Core/include/ITrackerFieldCatalog.h`, `Source/Core/include/Tracker/JiraClient.h` | `SupportsIssueTransitions()` |
| `tests/support/FakeTrackerClient.h` | override it (scriptable) |
| `Source/Core/include/AppController.h`, `AppController_CatalogAndFieldEdit.cpp`, `AppController_Connectivity.cpp` | query-based delegators; recovery hook |
| `Source/Core/src/TicketFieldEditor.cpp` | non-blocking combo + cue |
| `Source/Core/src/SmatchetLocalization.cpp` | 3 keys |
| `Source/Core/src/Persistence/AGENTS.md`, `Source/Core/src/Tracker/AGENTS.md` | invariants |
| tests: `LearnedWorkflowPure`, `StatusComboOptionsPure`, `IssueTransitionsCacheService`, `LookupCacheSqlite` + `tests/ui/offline_first.test.cpp` | NEW / extend |

### S5 steps

1. **`Persistence/ILookupCache.h`.** SQLite-free. Includes `<cstdint>`, `<string>` and `<vector>`.
   ```cpp
   struct LookupCacheRow { std::string CacheKey; std::string PayloadJson; std::int64_t UpdatedAtEpochSec = 0; };
   class ILookupCache {
     public:
       virtual ~ILookupCache() = default;
       virtual bool UpsertLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey, const std::string& payloadJson) = 0;
       virtual bool TryGetLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey, LookupCacheRow& out) = 0;
       virtual std::vector<LookupCacheRow> LoadLookups(const std::string& backendKey, const std::string& kind) = 0;
       virtual bool DeleteLookup(const std::string& backendKey, const std::string& kind, const std::string& cacheKey) = 0;
   };
   ```
   Add a header comment: persisted read-side lookups for offline use (Pillar 6); rows are keyed by
   backend namespace + kind + key; call only from workers.
2. **`LocalCacheManager`:**
   - Change `class LocalCacheManager : public ISyncCache {` to
     `class LocalCacheManager : public ISyncCache, public ILookupCache {`.
   - Add `#include "ILookupCache.h"` next to `#include "ISyncCache.h"`.
   - Declare the 4 overrides (same signatures plus `override`) and a private
     `void InitLookupCacheSchema_();`.
   - In `LocalCacheManager.cpp`, after the line `    InitFieldEditQueueSchema(db);` inside
     `InitSchema()`, add `    InitLookupCacheSchema_();`.
   - In the new TU `LocalCacheManager_Lookup.cpp`, `InitLookupCacheSchema_()` runs:
   ```cpp
   db.exec("CREATE TABLE IF NOT EXISTS lookup_cache ("
           "backend_key TEXT NOT NULL, kind TEXT NOT NULL, cache_key TEXT NOT NULL, "
           "payload_json TEXT NOT NULL, schema_version INTEGER NOT NULL DEFAULT 1, "
           "updated_at INTEGER NOT NULL, PRIMARY KEY (backend_key, kind, cache_key))");
   ```
   - The 4 methods use local `SQLite::Statement` objects, like `EnqueuePendingFieldEdit`:
     - Upsert = `INSERT OR REPLACE ... VALUES (?,?,?,?,1,?)` with the current unix time (`std::time(nullptr)`).
     - Get/Load `SELECT cache_key, payload_json, updated_at ...`.
     - Delete.
   - Wrap each body in `try { ... } catch (const std::exception& ex) { LOG_WARN("LocalCacheManager::<fn> failed: %s", ex.what()); return false/empty; }`.
   - Add the new TU next to every `LocalCacheManager.cpp` entry in the test CMake lists: first run
     `grep -rn "Persistence/LocalCacheManager.cpp" tests/CMakeLists.txt`.
3. **`IEditMetaDeps.h`:**
   - Forward-declare `class ILookupCache;`.
   - Add pure virtual `virtual std::shared_ptr<ILookupCache> LookupCacheShared() = 0;` with the comment
     "Latched strong handle; null before the cache exists."
   - Add `virtual std::string CacheBackendKey() const = 0;`.
   - Adapter: the existing `CacheBackendKey() const override` satisfies it. Add
     `std::shared_ptr<ILookupCache> LookupCacheShared() override;`, implemented as
     `return std::atomic_load(&app_.Cache);`. If the upcast from `shared_ptr<LocalCacheManager>` does
     not compile, include `Persistence/LocalCacheManager.h` in the adapter `.cpp`.
   - Fake: add `std::shared_ptr<ILookupCache> LookupCacheImpl; std::string CacheBackendKeyImpl = "Jira";`
     and the two overrides.
4. **`tests/support/FakeLookupCache.h`.** An `ILookupCache` over `std::map<std::string, LookupCacheRow>`
   keyed `backend + '\x1f' + kind + '\x1f' + key`, guarded by a `std::mutex`, with a public
   `int UpsertCalls`. Include it as `#include "ILookupCache.h"`, the same bare form
   `tests/support/FakeSyncCache.h` uses for `"ISyncCache.h"`. It must never include
   `LocalCacheManager.h`: the pure-sync test set bans SQLite.
5. **`Types/TransitionsTypes.h`.** Includes `"OfflineFirstPure.h"`, `"Tracker/TrackerFieldSchema.h"`,
   `<string>` and `<vector>`.
   ```cpp
   struct TransitionsQuery { std::string IssueId; std::string ProjectKey; std::string IssueTypeKey; std::string FromStatusKey; };
   struct TransitionsLookup {
       bool applicable = false;                       ///< backend supports transitions (Jira)
       std::vector<TrackerFieldOption> options;       ///< live if Fresh, else remembered, else empty
       smatchet::offline::DataFreshness freshness = smatchet::offline::DataFreshness::UnavailableNoCache;
       bool fromLearned = false;                      ///< options came from the remembered workflow
   };
   ```
   Remove `struct TransitionsLookup` from `IssueTransitionsCacheService.h`, which now includes this
   header.
6. **`LearnedWorkflowPure.{h,cpp}`**, in namespace `smatchet::workflow`:
   - `constexpr const char* kLearnedTransitionsKind = "workflow_transitions";`
   - `std::string BuildLearnedTransitionsKey(const std::string& projectKey, const std::string& issueTypeKey, const std::string& fromStatusKey);`
     returns `projectKey + "|" + issueTypeKey + "|" + fromStatusKey`, or `""` if any part is empty.
   - `std::string SerializeTransitionTargets(const std::vector<TrackerFieldOption>& options);` produces
     a JSON array of `{"id","name"}` via nlohmann `dump()`.
   - `bool ParseTransitionTargets(const std::string& json, std::vector<TrackerFieldOption>& out);`
     parses with `smatchet::json_safe::ParseBounded` from `Json/BoundedJsonParse.h`. It returns false
     on a parse error or non-array, and skips entries lacking both `id` and `name`.
   - Tests go in both lists; the TSan list also needs the production `.cpp`.
7. **`StatusComboOptionsPure.h`** (header-only), namespace `smatchet::statuscombo`:
   - `enum class StatusOptionsSource : unsigned char { Live, Learned, Catalog };`
   - `struct StatusComboPick { std::vector<TrackerFieldOption> Options; StatusOptionsSource From = StatusOptionsSource::Catalog; };`
   - `inline StatusComboPick PickStatusComboOptions(const std::vector<TrackerFieldOption>& targets, bool targetsAreLive, const std::vector<TrackerFieldOption>& catalogAll, const TrackerFieldOption& current)`:
     - uses `targets` when non-empty (source Live or Learned), else `catalogAll` (source Catalog);
     - then prepends `current` unless an option with the same Id-or-Value is already present;
     - skips the prepend when current's Id and Value are both empty.
8. **`ITrackerFieldCatalog.h`:** add
   `/// True when FetchIssueTransitions is implemented; callers skip the request (and its log) otherwise.`
   followed by `virtual bool SupportsIssueTransitions() const { return false; }`.
   - `JiraClient.h`: add `bool SupportsIssueTransitions() const override { return true; }`.
   - `FakeTrackerClient.h`: add `bool SupportsIssueTransitions() const override { return supportsIssueTransitions_; }`
     and `void SetSupportsIssueTransitions(bool on)`, default `false`.
9. **`IssueTransitionsCacheService` rewrite.** Keep the header's lifetime comment and add one sentence
   on Pillar 6.
   - **Public API:**
     - the constructor, unchanged;
     - `TransitionsLookup GetAvailableTransitions(const TransitionsQuery& q) const;`
     - `void EnsureIssueTransitionsLoaded(const TransitionsQuery& q, const TrackerConfig* configSnapshot = nullptr);`
     - `void InvalidateIssueTransitions(const std::string& issueId);`
     - `void OnConnectivityRecovered();`
   - **Private members:**
     - `static std::string LiveKey(const std::string& backendKey, const std::string& issueId)`,
       returning `backendKey + "|" + issueId`;
     - `void EnsureLearnedLoaded(const std::string& backendKey)`;
     - `void RememberLearned(const std::string& backendKey, const TransitionsQuery& q, const std::vector<TrackerFieldOption>& options, const std::shared_ptr<ILookupCache>& store)`;
     - `IEditMetaDeps& deps_; smatchet::offline::KeyedLookupCache<std::vector<TrackerFieldOption>> live_; mutable std::mutex learnedMutex_; std::unordered_map<std::string, std::vector<TrackerFieldOption>> learned_; std::unordered_set<std::string> learnedLoadedBackends_;`
   - **Behaviour:**
     - **Applicability:** `applicable` = backend present, `FieldCatalog()` non-null and
       `SupportsIssueTransitions()`. When it is false, `Ensure` returns immediately: no fetch, no log.
     - **`Get`:**
       - If the live entry has `HasValue && Live`, return those options with `live_.Freshness(...)`.
       - Otherwise look up `learned_[backendKey + "|" + BuildLearnedTransitionsKey(project, type, from)]`
         and set `fromLearned` if found.
       - The freshness comes from `ClassifyFreshness` with `HasCache = !options.empty()`,
         `Live = false`, `InFlight`/`LastAttemptFailed` taken from the live entry, and
         `Connectivity = deps_.TrackerConnectivity()`.
     - **`Ensure`:**
       - calls `EnsureLearnedLoaded(backendKey)`, then
         `live_.TryBeginFetch(LiveKey(...), deps_.TrackerConnectivity(), Clock::now(), ticket)`;
       - on true, captures `backend`, `catalog`, `ticket`, `q`, `backendKey`,
         `store = deps_.LookupCacheShared()`, `haveCfg`/`cfg` and `this` into
         `deps_.LaunchBackgroundTask`;
       - the worker reads `ConfigManager::Load()` there when no snapshot was passed, then calls
         `smatchet::offline::RunKeyedFetch(live_, ticket, [&]() { auto r = catalog->FetchIssueTransitions(useCfg, q.IssueId); if (r.has_value()) { RememberLearned(backendKey, q, r.value(), store); } else { LOG_DEBUG("IssueTransitionsCacheService: transitions fetch failed issue=%s kind=%s", q.IssueId.c_str(), ToString(r.error().Kind)); } return r; });`.
     - **`EnsureLearnedLoaded`:** returns if the store is null or the backend is already in
       `learnedLoadedBackends_` (insert under the lock). Otherwise it launches a worker that calls
       `store->LoadLookups(backendKey, kLearnedTransitionsKind)`, parses each row, then, under the lock,
       `learned_.emplace(backendKey + "|" + row.CacheKey, opts)`. `emplace` never overwrites a value
       learned meanwhile.
     - **`RememberLearned`:**
       - returns if any query part or `options` is empty;
       - returns if any option's `Id` equals `q.FromStatusKey` (the server's status moved; do not
         learn a wrong edge);
       - otherwise stores into `learned_` under the lock, then calls
         `store->UpsertLookup(backendKey, kLearnedTransitionsKind, key, SerializeTransitionTargets(options))`
         when the store is non-null.
     - **`InvalidateIssueTransitions`:** `live_.Invalidate(LiveKey(deps_.CacheBackendKey(), issueId))`.
     - **`OnConnectivityRecovered`:** `live_.OnConnectivityRecovered()`.
10. **AppController:**
    - Declarations become
      `struct TransitionsLookup GetAvailableTransitionsForIssue(const TransitionsQuery& query) const;`
      and `void EnsureIssueTransitionsLoaded(const TransitionsQuery& query) const;`. Add a
      `struct TransitionsQuery;` forward declaration next to `struct TransitionsLookup;`.
    - The definitions in `AppController_CatalogAndFieldEdit.cpp` forward the query.
    - `AppController_Connectivity.cpp`: add `#include "IssueTransitionsCacheService.h"` and replace
      `ConsumeTrackerConnectivityRecovery()`'s body with:
      ```cpp
      const bool recovered = connectivity_ ? connectivity_->ConsumeTrackerConnectivityRecovery() : false;
      if (recovered && transitions_) {
          transitions_->OnConnectivityRecovered();
      }
      return recovered;
      ```
    - Run `grep -rn "GetAvailableTransitionsForIssue\|EnsureIssueTransitionsLoaded" Source tests`. Only
      `TicketFieldEditor.cpp` and the AppController files may call them; otherwise STOP.
11. **`TicketFieldEditor.cpp`:**
    - Add the includes `"StatusComboOptionsPure.h"`, `"DataFreshnessCue.h"` and
      `"SmatchetLocalization.h"` if missing. Check with grep.
    - In the anonymous namespace, before `RenderSingleSelectEditor`, add:
    ```cpp
    // First combo row when the status list is not the live transition set (Pillar 6 freshness cue).
    void DrawStatusComboCue(smatchet::statuscombo::StatusOptionsSource from, smatchet::offline::DataFreshness freshness) {
        const char* text = nullptr;
        if (freshness == smatchet::offline::DataFreshness::Refreshing ||
            freshness == smatchet::offline::DataFreshness::LoadingNoCache) {
            text = SmatchetLocalization::T("status.cue.checking", "Checking valid transitions\xE2\x80\xA6");
        } else if (from == smatchet::statuscombo::StatusOptionsSource::Learned) {
            text = SmatchetLocalization::T("status.cue.cached_workflow", "Saved workflow (last seen online)");
        } else {
            text = SmatchetLocalization::T("status.cue.all_statuses_offline",
                                           "All statuses shown \xE2\x80\x94 an invalid move is rejected when it syncs");
        }
        ImGui::TextDisabled("%s", text);
        ImGui::Separator();
    }
    ```
    - Inside `if (comboOpened) {` in `RenderSingleSelectEditor`, replace everything from
      `        // For status field on Jira, filter to valid transitions + current status.` through the
      closing `}` of the `if (field.Id == "status" && !transitionsLoaded) { ... } else { ... }` block
      (keep the following `ImGui::EndCombo();`) with:
    ```cpp
            const std::vector<TrackerFieldOption>* opts = &field.AllowedValueOptions;
            std::vector<TrackerFieldOption> statusOptions;
            if (field.Id == "status") {
                // Pillar 6: never block on the live transitions fetch. Live transitions, else the workflow
                // remembered from earlier online use, else every status; the current status always shows.
                const std::string currentId = ResolveOptionId(field, currentValue);
                TransitionsQuery query;
                query.IssueId = ticket.id;
                query.ProjectKey = smatchet::ExtractIssueKeyPrefix(ticket.id);
                query.IssueTypeKey = ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
                query.FromStatusKey = currentId;
                app.EnsureIssueTransitionsLoaded(query);
                const TransitionsLookup lookup = app.GetAvailableTransitionsForIssue(query);
                if (lookup.applicable) {
                    TrackerFieldOption current;
                    current.Id = currentId;
                    current.Value = app.ResolveDisplayValue(field.Id, &field, currentValue);
                    if (current.Value.empty()) {
                        current.Value = currentId;
                    }
                    const bool live = lookup.freshness == smatchet::offline::DataFreshness::Fresh;
                    smatchet::statuscombo::StatusComboPick pick = smatchet::statuscombo::PickStatusComboOptions(
                        lookup.options, live, field.AllowedValueOptions, current);
                    statusOptions = std::move(pick.Options);
                    opts = &statusOptions;
                    if (pick.From != smatchet::statuscombo::StatusOptionsSource::Live) {
                        DrawStatusComboCue(pick.From, lookup.freshness);
                    }
                }
            }
            RenderSingleSelectComboBody(ticket, field, currentValue, state, pendingEdits, editorKey,
                                        opts->empty() && app.FieldCatalogLacksProjectScope(), opts);
    ```
    - Run `grep -n "Loading transitions" Source`; it must return nothing.
12. **Localization.** Insert these after the `comments.fetch_failed` line, after first grepping to make
    sure none of the keys exists:
    ```cpp
    {"status.cue.checking", "Checking valid transitions\xE2\x80\xA6", u8"Vérification des transitions…"},
    {"status.cue.cached_workflow", "Saved workflow (last seen online)", u8"Flux enregistré (vu en ligne)"},
    {"status.cue.all_statuses_offline", "All statuses shown \xE2\x80\x94 an invalid move is rejected when it syncs", u8"Tous les statuts affichés — un changement invalide sera refusé à la synchronisation"},
    ```
13. **Leaf docs:**
    - `Persistence/AGENTS.md` `## Invariants`: add
      `- **Offline read-side lookups persist in \`lookup_cache\`.** Store them as (backend_key, kind, cache_key) → JSON rows through \`ILookupCache\` (backend-namespaced like the ticket tables); add a new \`kind\` rather than a new ad-hoc JSON file. Reads and writes run on workers; the owning service keeps an in-memory copy for the UI thread.`
    - `Tracker/AGENTS.md`: add
      `- **An optional backend feature exposes a capability flag** (e.g. \`ITrackerFieldCatalog::SupportsIssueTransitions()\`) so callers skip the request instead of logging a warning per issue.`
14. **Tests:**
    - **`LearnedWorkflowPure.test.cpp`** (both lists): key building (including an empty part), a
      serialize → parse round trip, malformed JSON returning false, and an entry with only `name`.
    - **`StatusComboOptionsPure.test.cpp`** (both lists): live targets plus current prepended, learned
      targets, the catalog fallback, current already present (no duplicate), never empty when current
      is set, and an id-less option matched by value.
    - **`IssueTransitionsCacheService.test.cpp`** (TSan list with `IssueTransitionsCacheService.cpp` +
      `LearnedWorkflowPure.cpp`; SmatchetTests list):
      - Setup: `FakeEditMetaDeps deps` with `RunOnRealThread=false`. Cast `deps.BackendImpl` to
        `FakeTrackerClient` and call `SetSupportsIssueTransitions(true)`. Set
        `deps.LookupCacheImpl = std::make_shared<FakeLookupCache>()`.
      - Offline → 0 fetches; `applicable` is true and `options` empty.
      - Online success → Fresh, 1 upsert with key `"PROJ|bug|1"`.
      - A new service with the pre-seeded store while offline → `fromLearned` is true and freshness is
        `CachedOffline`.
      - Transport failure → a second `Ensure` does not fetch; after `OnConnectivityRecovered` it does.
      - `SupportsIssueTransitions=false` → 0 fetches and `applicable=false`.
      - A throwing fetch → `CHECK_THROWS`, then freshness is not `Refreshing`.
      - A fetched list containing the from-status → no upsert.
    - **`LookupCacheSqlite.test.cpp`** (SmatchetTests only): use the temp-DB pattern of the existing
      `LocalCacheManager` tests.
      - upsert/get/load/delete round trip;
      - upsert replaces;
      - backend namespaces are isolated;
      - opening a DB created before this change (no table) works, because `CREATE IF NOT EXISTS`
        adds it.
    - **Bucket E**, add to `tests/ui/offline_first.test.cpp`:
      `IM_REGISTER_TEST(engine, "OfflineFirst", "StatusCombo_OfflineShowsOptions")`. This drives the
      service through the app API, so no ImGui cell IDs are needed.
      1. `ScopedFakeNetworkReset reset;`
      2. Build `TransitionsQuery q{"OFF-1", "OFF", "bug", "1"}`, where `"1"` is the `To Do` option id
         from the fixture.
      3. `app->EnsureIssueTransitionsLoaded(q);` then `ctx->Yield(30);`. Assert
         `app->GetAvailableTransitionsForIssue(q).freshness == DataFreshness::Fresh` and that `options`
         has 2 entries.
      4. Set `TransportDown`. Build a query for `OFF-2` (same project, type and status — never fetched).
         `Ensure` + `Yield(10)`. Assert `options` has the learned 2 entries and `fromLearned`.
      5. Build a query with type `"task"`, which has no learned row. Assert `options` is empty and
         `applicable`, which means the combo falls back to the full catalog list; that fallback is unit
         tested in `StatusComboOptionsPure`.
      6. Assert `smatchet_tests::GlobalFakeNetwork().CallsWhileDown() == 0`.

**Verify:** Linux `--test-case='LearnedWorkflow*,StatusComboOptions*,IssueTransitionsCache*,KeyedLookupCache*'`; CI bucket-E `OfflineFirst`.
**Done when:** the combo never shows a loading-only state; learned transitions survive a restart (the
LookupCacheSqlite test); the offline bucket-E test is green.

---

## S6 — fix(field-edit): queue field edits immediately when offline; status edits are queueable

**Symptom for the Issue:** "An offline status change fails instead of queueing. Other offline edits
wait out 2–3 HTTP retry windows in RAM before they are saved. Linear edits never queue."

**Files:**
- `FieldEditPipelineService.{h,cpp}`, `IFieldEditDeps.h`, `GridContextDepsAdapter.{h,cpp}`
- `tests/support/FakeFieldEditDeps.h`
- `AppController.h` + `AppController_CatalogAndFieldEdit.cpp` (delegators)
- `Ui/SmatchetUiSession.h` (`FieldEditCommitResult`)
- `SmatchetGridFieldEditPipeline.cpp`
- `Sync/OfflineQueueService.cpp` (worker-safe enqueue)
- `EditMetaCacheService.{h,cpp}`
- `Tracker/LinearIssueMutation.cpp`
- `Source/Core/src/Sync/AGENTS.md`, `agents/core/code-review.md`
- tests: `FieldEditPipelineService.test.cpp`, `EditMetaCacheService.test.cpp`,
  `LinearIssueMutationHttp.test.cpp`, `tests/ui/offline_first.test.cpp`

### S6 steps

1. **Status queueable.** In `FieldEditPipelineService::FieldEditSupportsOfflineQueue`, add
   `    case TrackerFieldFamily::Status:` on the line before `    case TrackerFieldFamily::CascadingSelect:`.
2. **No network lookups when preparing a queued edit.**
   - Add a trailing `bool allowNetworkLookups = true` parameter to `TryBuildFieldEditPayloadForNetwork`
     and `TryPrepareOfflineFieldEdit` (header + `.cpp`). `TryPrepareOfflineFieldEdit` forwards it.
   - In `TryBuildFieldEditPayloadForNetwork`, wrap the existing
     `editMeta_.EnsureIssueEditMetaLoaded(issueId, issueTypeKeyOpt);` statement (inside its `if`) as
     `if (allowNetworkLookups) { … }`.
   - `CanEditFieldForIssue` stays and is optimistic when nothing is loaded.
   - If `AppController::TryPrepareOfflineFieldEdit` delegates, give it the same defaulted parameter and
     forward it.
3. **Worker-safe enqueue.**
   - `IFieldEditDeps.h`: add
     `virtual std::int64_t EnqueueOfflineFieldEdit(const std::string& issueKey, const std::string& fieldId, const std::string& fieldsPayloadJson, const std::string& originalRichValue, const std::string& originalValue, bool hasOriginalValue, std::string& outError) = 0;`
     with the comment "Worker-safe: persists to the offline queue (SQLite) off the UI thread." Also
     update the header comment that says this service exposes no `QueueFieldEditOffline`.
   - Adapter:
     `return app_.QueueFieldEditOffline(issueKey, fieldId, fieldsPayloadJson, outError, originalRichValue, originalValue, hasOriginalValue);`.
   - Fake: record the call in a public vector and return an incrementing id, or `0` with a scripted
     error when `EnqueueFailImpl` is set.
   - `OfflineQueueService::QueueFieldEditOffline`:
     - Replace the two uses of `deps_.Cache()` with one latched
       `std::shared_ptr<ISyncCache> cache = deps_.CacheShared();`, taken right after the read-only
       check, in the same style as `QueueCreateOffline`'s DR6 comment.
     - Null-check `cache` where `!deps_.Cache()` was checked.
     - Call `cache->EnqueuePendingFieldEdit(...)`.
     - Add the comment "Runs on the field-edit worker (Pillar 6 queue-first) — latch the cache once."
4. **One commit-or-queue seam.** In `FieldEditPipelineService.h`, declare:
   ```cpp
   struct FieldEditCommitRequest {
       std::string IssueId; TrackerField Field; std::vector<std::string> Values;
       std::string OriginalRichValue; std::string OriginalValue; bool HasOriginalValue = false;
       std::string OriginalEstimateSnapshot; std::string RemainingEstimateSnapshot; std::string IssueTypeKeySnapshot;
       TrackerConnectivityState ConnectivityAtKick = TrackerConnectivityState::Unknown;
   };
   enum class FieldEditCommitKind : unsigned char { Failed, SavedOnline, QueuedOffline };
   struct FieldEditCommitOutcome {
       FieldEditCommitKind Kind = FieldEditCommitKind::Failed; FieldEditResult Apply; std::int64_t QueueId = 0;
       bool QueuedAfterTransportFailure = false; std::string Error;
   };
   /// Worker-safe. Offline (per ConnectivityAtKick) a queueable edit goes straight to the queue with no network;
   /// online it tries the network first and queues on a retryable failure. Never blocks on editmeta when queueing.
   FieldEditCommitOutcome CommitOrQueue(const FieldEditCommitRequest& req);
   ```
   Also declare a private helper
   `FieldEditCommitOutcome QueuePreparedEdit(const FieldEditCommitRequest& req, bool afterTransportFailure);`.
   - **Implementation:**
     - `RouteWrite(req.ConnectivityAtKick, FieldEditSupportsOfflineQueue(req.Field), ConfigManager::Load().ReadOnlyMode)`.
     - `Reject` → `Error = "Read-only mode is enabled in Preferences."`.
     - `QueueImmediately` → `QueuePreparedEdit(req, false)`.
     - Otherwise:
       ```
       Apply = SubmitFieldEditNetworkOnly(...);
       if Ok → SavedOnline;
       else if Apply.ErrorTransient && supported → q = QueuePreparedEdit(req, true); return q if QueuedOffline else keep the network error (or the queue error if non-empty);
       else Failed with Apply.Error.
       ```
     - `QueuePreparedEdit` calls `TryPrepareOfflineFieldEdit(..., /*allowNetworkLookups=*/false)`,
       then `deps_.EnqueueOfflineFieldEdit(...)`. On success it returns QueuedOffline with the prepared
       `Apply`; on failure it returns Failed with the error.
   - Keep each function ≤ 60 lines.
   - AppController: add `FieldEditCommitOutcome CommitOrQueueFieldEdit(const FieldEditCommitRequest& req);`,
     which forwards to `fieldEdit_`.
     - In `AppController.h`, only forward-declare `struct FieldEditCommitRequest;` and
       `struct FieldEditCommitOutcome;`. Do NOT add an include there; it is included almost everywhere.
     - Define the method in `AppController_CatalogAndFieldEdit.cpp`, which includes
       `FieldEditPipelineService.h`.
     - `SmatchetGridFieldEditPipeline.cpp` must `#include "FieldEditPipelineService.h"` to use the
       types.
5. **Grid.**
   - `SmatchetUiSession.h` `FieldEditCommitResult`: remove `QueuedFieldsPayloadJson` and its comment,
     and add `bool QueuedAfterTransportFailure = false;`.
   - `RunCommitWorker`: take a new first parameter
     `TrackerConnectivityState connectivityAtKick`. Replace the body up to the `PostToMainThread` call
     with:
     - build `FieldEditCommitRequest req` from `edit` plus the three snapshots and `connectivityAtKick`;
     - `const FieldEditCommitOutcome o = app.CommitOrQueueFieldEdit(req);`;
     - map it onto `result`: `CommitKind`, `ApplyResult = o.Apply`, `Ok = (o.Kind != Failed)`,
       `Error = o.Error`, `QueuedAfterTransportFailure`.
   - `PumpGridFieldEdits`: capture `const TrackerConnectivityState connectivityAtKick = app.GetLastTrackerConnectivityState();`
     before `LaunchBackgroundTask` and pass it through.
   - `ApplyCommitResultOnUiThread`: in the `QueuedOffline` branch, delete the
     `app.QueueFieldEditOffline(...)` call and the `qid <= 0` branch. It now starts with
     `if (!ApplyFieldEditResultBool(...))`, keeping the other two branches as they are. After the
     success toast, add `if (result.QueuedAfterTransportFailure) { app.RequestTrackerProbeNow(); }`.
6. **Editmeta: offline gate and backoff** (`EditMetaCacheService`).
   - Add `std::chrono::steady_clock::time_point retryAfter{};` to `IssueEditMetaCache` and include
     `<chrono>`.
   - In `EnsureIssueEditMetaLoaded`, immediately before the line
     `    const TrackerConfig cfg = configSnapshot ? *configSnapshot : ConfigManager::Load();` that is
     directly followed by `    std::unordered_map<std::string, bool> meta;`, insert:
     ```cpp
     if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) {
         return VoidResult::Err("Tracker is offline; edit permissions were not refreshed.");
     }
     ```
   - In the same function, set `cache.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(smatchet::offline::kLookupRetryAfterSeconds);`
     when `!ok`.
   - In `WarmIssueEditMetaAsync`, inside the existing lock block after the `loaded` check, add:
     ```cpp
     if (it != issueEditMeta_.end() && !it->second.loaded && std::chrono::steady_clock::now() < it->second.retryAfter) {
         return; // failed recently — back off instead of refetching every frame
     }
     ```
     Before taking the lock, add `if (smatchet::offline::IsOfflineState(deps_.TrackerConnectivity())) { return; }`.
   - Include `"OfflineFirstPure.h"`.
7. **Linear transport classification** (`Tracker/LinearIssueMutation.cpp`, updates only; do not touch
   create or comment POSTs, because of the DR16 double-post risk).
   - Add a trailing `long* outStatus = nullptr` to `RunLinearMutation` and `ResolveIssueUuid`. Both are
     file-local helpers in `LinearIssueMutation.cpp` with no header declaration; verify with
     `grep -rn "RunLinearMutation\|ResolveIssueUuid" Source`. Each sets
     `if (outStatus) { *outStatus = resp.status_code; }` right after its `TrackerPostLogged` call.
     Existing callers stay unchanged.
   - In `LinearClient::UpdateIssueFields`, pass `&resolveStatus` / `&mutationStatus`, both initialised
     to `200`.
   - Replace the two `return TrackerErrorInvalidRequest(resolveError);` /
     `return TrackerErrorInvalidRequest(outError);` returns with:
     `return resolveStatus != 200 ? ClassifyRejectedHttpStatus(resolveStatus, resolveError) : TrackerErrorInvalidRequest(resolveError);`
     and the same shape for the mutation.
8. **Docs:**
   - `Sync/AGENTS.md` `## Invariants`: add
     `- **Queue first when offline.** When the connectivity probe says the tracker is unreachable (\`IsOfflineState\`), a write persists to the queue immediately (\`RouteWrite\` → \`QueueImmediately\`, e.g. \`FieldEditPipelineService::CommitOrQueue\`) instead of first spending the HTTP retry window; online, a retryable failure still falls back to the queue.`
   - `code-review.md` Offline-first Write bullet: change "it reaches the offline queue" to
     "it reaches the offline queue (`FieldEditPipelineService::CommitOrQueue` for field edits)".
9. **Tests:**
   - **`FieldEditPipelineService.test.cpp`:**
     - Status is queueable.
     - `CommitOrQueue` offline: 0 `UpdateIssueFields` calls, 0 `FetchIssueEditMeta` calls, 1 enqueue,
       and the kind is QueuedOffline.
     - Online with a scripted Transport error: 1 update call, then enqueue with
       `QueuedAfterTransportFailure`.
     - Online with an InvalidRequest error: Failed and no enqueue.
     - An enqueue failure gives Failed with the enqueue error.
   - **`EditMetaCacheService.test.cpp`:**
     - offline → `EnsureIssueEditMetaLoaded` makes 0 fetches;
     - after a scripted failure, `WarmIssueEditMetaAsync` does not refetch.
   - **`LinearIssueMutationHttp.test.cpp`:** add a case where the loopback server is replaced with an
     unreachable port (`http://127.0.0.1:1`) → `UpdateIssueFields` returns `Transport` and
     `IsRetryable()`. Follow the file's existing server/config setup.
   - **Bucket E**, `StatusEdit_OfflineQueuesThenReplays`. Drive the real grid pipeline the way the
     `debug.grid.edit-burst` command does (`Commands/Builtin/BuiltinCommands_Perf.cpp`
     `RunGridEditBurstOnUi`): include `"SmatchetGridUiSupport.h"` and `"SmatchetUiSession.h"`, which
     declares `g_ui`.
     1. `ScopedFakeNetworkReset reset;` then set `TransportDown`.
     2. Wait until `app->GetLastTrackerConnectivityState()` is `TransportDown`. Poll with `ctx->Yield(1)`
        up to 600 frames; the probe interval may need `app->RequestTrackerProbeNow()` first.
     3. Build a `PendingFieldEdit` for `OFF-1`: `Field = *app->FindFieldById("status")`,
        `Values = {"2"}`, `OriginalValue` = the ticket's current status, `HasOriginalValue = true`.
     4. Call `ProcessGridFieldEdits(*app, g_ui, tickets, {edit}, false)` and `ctx->Yield(60)`.
     5. Assert `app->GetPendingFieldEdits().size() == 1` and that the fake recorded 0
        `UpdateIssueFields` calls. The worker never reached the network: check
        `CallsWhileDown()` did not grow beyond the probe-free count.
     6. Set `Up`, then call `app->RequestTrackerProbeNow()` so the next frame probes. The frame loop
        then sees recovery, restarts the replay timers and ticks replay.
     7. Yield ≤ 900 frames until `GetPendingFieldEdits().empty()`. Assert the fake recorded exactly 1
        `UpdateIssueFields` call with a `status` payload.
     8. If the queue does not drain, STOP and report the connectivity state and queue row. Do not add
        sleeps.

**Verify:** Linux `--test-case='FieldEditPipelineService*,EditMetaCacheService*,OfflineQueue*'`; CI `LinearIssueMutationHttp`, `OfflineFirst`.
**Done when:** an offline status change shows "Queued" without waiting and replays on reconnect.

---

## S7 — fix(comments): comments modal shows cached thread offline; truthful posting state `[visual]`

**Symptom for the Issue:** "Comments tooltip shows cached comments offline but the opened comments view
is stuck on 'Loading comments...' and then shows 'No comments yet.'; the post toast says 'Comment
Queued' but nothing is queued."

**Files:**
- `Tracker/CommentBlobFormatPure.{h,cpp}`
- `Tracker/JiraIssueMappingPure.cpp`
- `AppController_CatalogAndFieldEdit.cpp` (`UpdateCachedCommentsFromThread`, `FetchIssueCommentsTyped`)
- `AppController.h`
- `Sync/LazyEnrichmentCarryForwardPure.h` (NEW)
- `Sync/TicketSyncService.cpp`
- `Ui/SmatchetCommentsModalSeedPure.h` (NEW)
- `Ui/SmatchetCommentsModalUi.cpp`
- `Ui/SmatchetActiveProjectGridCells.cpp`
- `SmatchetLocalization.cpp`
- `Source/Core/src/Ui/AGENTS.md`
- tests: `CommentBlobFormatPure.test.cpp`, `LazyEnrichmentCarryForwardPure.test.cpp` (NEW),
  `SmatchetCommentsModalSeedPure.test.cpp` (NEW), `tests/ui/offline_first.test.cpp`

### S7 steps

1. **Structured thread.** In `CommentBlobFormatPure.h/.cpp` (namespace `smatchet::tracker`), add:
   - `constexpr const char* kCommentThreadRichKey = "comment_thread";`
   - `std::string SerializeCommentThread(const std::vector<TrackerIssueComment>& comments);`: a JSON
     array of `{id, author, body, created, updated}`, oldest first. It keeps the newest 50 and drops
     the oldest until `dump()` is ≤ 64 KiB. Wrap the nlohmann dump in try/catch and return `""` on
     failure. This TU is Logger-free, so do not log.
   - `bool ParseCommentThread(const std::string& json, std::vector<TrackerIssueComment>& out);`: uses
     `json_safe::ParseBounded`; false on a parse error or non-array.
   - `std::vector<TrackerIssueComment> ParseCommentBlob(const std::string& blob);`: the inverse of
     `FormatCommentBlob` for legacy rows.
     - Entries are separated by a blank line.
     - The header is `[Author] YYYY-MM-DD`; parse the date to midnight UTC epoch with a hand-rolled
       days-from-civil calculation (no `timegm`).
     - Everything after the header line is the body.
     - The result is oldest-first (reverse the blob's newest-first order).
2. **Writers.**
   - `UpdateCachedCommentsFromThread`:
     - also compute `newThread = SerializeCommentThread(comments)`;
     - include `ticket.GetFieldRichValue(kCommentThreadRichKey) == newThread` in the no-change check;
     - set `updated.fieldRichValues[kCommentThreadRichKey] = newThread`.
   - `JiraIssueMappingPure.cpp` `MapJiraPresentField`: in the `fieldKey == "comment"` branch, also set
     `ticket.fieldRichValues[smatchet::tracker::kCommentThreadRichKey]`. The value is
     `SerializeCommentThread(MapJiraIssueComments(commentsArray))` when `commentsArray` is a non-empty
     array; otherwise leave it unset. Do not change the existing blob logic.
3. **Carry-forward** (rich values are deleted on every ticket save).
   - NEW `Sync/LazyEnrichmentCarryForwardPure.h` (header-only):
     `inline void CarryForwardLazyCommentFields(const CachedTicket& prev, CachedTicket& incoming)`.
     - When `incoming` lacks `fieldRichValues["comment_thread"]` and has the same `fieldValues["comments"]`
       count as `prev`, copy prev's `comment_thread`.
     - When `incoming.fieldValues["comment"]` is empty and prev's is not, and the counts are equal, copy
       it too.
   - In `TicketSyncService.cpp`, directly before
     `            deps_.Cache()->SaveTickets(deps_.CacheBackendKey(), batchToProcess);`:
     - build an `unordered_map<std::string, const CachedTicket*>` of the current active tickets under
       `std::lock_guard<std::mutex> lk(deps_.ActiveTicketsMutex());`;
     - copy the needed previous tickets out while holding the lock;
     - call `CarryForwardLazyCommentFields(prev, t)` for each `t` in `batchToProcess` whose id matches.
   - Do the same before the `SaveTicket` loop in `ApplyIssueFetchPack`.
   - Keep the lock scope minimal: never hold it across `SaveTickets`.
4. **Typed fetch.** Add `Result<std::vector<TrackerIssueComment>, TrackerError> FetchIssueCommentsTyped(const std::string& issueKey);`
   to `AppController`, returning the backend `Result` unflattened (with the same backend/collaboration
   null checks mapped to `TrackerErrorInvalidRequest`). The existing `FetchIssueComments` becomes
   `return CollaborationResultToResult(FetchIssueCommentsTyped(issueKey));`, keeping its log.
5. **Seed picker.** NEW `Ui/SmatchetCommentsModalSeedPure.h`:
   `inline bool PickCommentsSeed(const std::string& threadJson, const std::string& blob, std::vector<TrackerIssueComment>& out, bool& outPartial)`.
   - Prefer the thread; `outPartial = false`.
   - Else use `ParseCommentBlob(blob)` with `outPartial = true`.
   - Return false when both are empty.
6. **Modal** (`SmatchetCommentsModalUi.cpp`).
   - **State:** add `bool Seeded = false; bool SeedPartial = false; bool FetchFailed = false; TrackerErrorKind FetchErrorKind = TrackerErrorKind::None;`
     to `CommentsModalState`.
   - **`OpenCommentsModal`:** after resetting state and before the fetch kick:
     - copy `seedThread = ticket.GetFieldRichValue(kCommentThreadRichKey)` and
       `seedBlob = ticket.GetFieldValue("comment")` from `app.GetActiveTicketsSnapshot()`, found by id
       in memory (no SQLite);
     - replace `FetchInFlight = true; KickCommentsFetch(...)` with `KickCommentsLoad(app, issueId, gen, seedThread, seedBlob);`.
   - **`KickCommentsLoad`** (renamed and extended `KickCommentsFetch`). It launches one worker that:
     1. parses the seed with `PickCommentsSeed` and posts it (Gen-guarded: sets `Comments`, `Seeded`,
        `SeedPartial`);
     2. if `!appPtr->IsTrackerOffline()`, calls `FetchIssueCommentsTyped` and posts the result. On
        success: set `Comments`, `Seeded=false`, `FetchFailed=false`, call
        `UpdateCachedCommentsFromThread`. On failure: `FetchFailed=true`, `FetchErrorKind`, `Error`
        (kept for the tooltip), and, **inside that main-thread post-back lambda** (never on the worker,
        because `nextProbeAt_` is UI-thread state), call `appPtr->RequestTrackerProbeNow()` when the
        kind is Transport;
     3. else posts `FetchFailed=true, FetchErrorKind=Transport` without fetching.
   - **Latch order and exit guard:**
     - Set `FetchInFlight = true` only after `app.LaunchBackgroundTask(...)` returns, inside
       `try { ... } catch (const std::exception& ex) { LOG_WARN(...); FetchInFlight = false; FetchFailed = true; }`.
     - In the worker, `smatchet::ScopeExit` posts a `FetchInFlight = false` completion when nothing was
       posted (including on a throw).
     - Every post clears `FetchInFlight` only on its final message.
   - **Render**, replacing `if (FetchInFlight) { Loading } else { DrawCommentsThread(); }`:
     - compute
       `f = ClassifyFreshness({HasCache: !Comments.empty() || (!FetchInFlight && !FetchFailed), Live: !Seeded && !FetchFailed, InFlight: FetchInFlight, LastAttemptFailed: FetchFailed, Connectivity: app.GetLastTrackerConnectivityState()})`;
     - `DataFreshnessCue::Draw(f, Error.empty() ? nullptr : Error.c_str())`;
     - if `SeedPartial`, draw the `comments.cached_partial` hint;
     - if `ShouldRenderContent(f)`, `DrawCommentsThread()`;
     - else, if `f == LoadingNoCache`, draw `comments.loading`;
     - otherwise draw `comments.unavailable_offline` plus a Retry button that re-runs `KickCommentsLoad`.
     - Delete the red `Error` block at the top. The error now goes in the cue tooltip.
     - `DrawCommentsThread` shows "No comments yet." only when `f == Fresh && Comments.empty()`.
   - **Posting:**
     - Change the toast title key `comments.queued_title` / "Comment Queued" to `comments.posting_title`
       / "Posting comment".
     - In `DrawCommentsPostBox`, add `const bool offline = app.IsTrackerOffline();`, disable the button
       while `offline`, and show `comments.post_offline_hint`. The draft is kept. S8 replaces this with
       queueing.
7. **Tooltip:** in `SmatchetActiveProjectGridCells.cpp`, change `if (commentBlob.empty() && pane.focused) {`
   to `if (commentBlob.empty() && pane.focused && !app.IsTrackerOffline()) {`.
8. **Localization** (grep first to make sure none exists):
   - `comments.posting_title` "Posting comment" / "Publication du commentaire"
   - `comments.cached_partial` "Showing a saved summary (latest 20 comments)" / "Résumé enregistré affiché (20 derniers commentaires)"
   - `comments.unavailable_offline` "Comments are not available offline yet." / "Commentaires pas encore disponibles hors ligne."
   - `comments.post_offline_hint` "Offline — your comment stays here until the tracker is reachable." / "Hors ligne — votre commentaire reste ici jusqu'à la reconnexion."
   - Leave `comments.queued_title` in the table; S8 uses it.
9. **Ui leaf doc:** add to `## Offline-first reads (Pillar 6)`:
   `- **Toasts say what happened.** "Queued" only after a real enqueue, "Posting…" while a request is in flight; never "Queued" for a direct post.`
10. **Tests:**
    - **`CommentBlobFormatPure.test.cpp`:**
      - thread round trip;
      - the 50-comment cap;
      - the 64 KiB cap;
      - `ParseCommentBlob` inverts `FormatCommentBlob` (author, date day, body, order), including
        multi-line bodies.
    - **`LazyEnrichmentCarryForwardPure.test.cpp`** (both lists): same count → copied; different count
      → not copied; incoming already has the thread → untouched.
    - **`SmatchetCommentsModalSeedPure.test.cpp`** (both lists): thread preferred, blob fallback marks
      partial, both empty → false.
    - **Test hook.** In `SmatchetCommentsModalUi.h`/`.cpp`, add
      `struct CommentsModalSnapshot { bool Active = false; bool FetchInFlight = false; bool Seeded = false; bool FetchFailed = false; std::size_t CommentCount = 0; std::string FirstAuthor; };`
      and `CommentsModalSnapshot GetCommentsModalSnapshotForTests();`, which copies from
      `s_CommentsState`. It is UI-thread only; say so in a comment.
    - **Bucket E**, `Comments_OfflineShowsCachedThread`:
      1. `ScopedFakeNetworkReset reset;`; run one online sync first so the fixture's comment blob and
         thread are in the cache (`app->SyncWithBackend(); ctx->Yield(120);`).
      2. Set `TransportDown` and wait for the state as in S6.
      3. `OpenCommentsModal(*app, "OFF-1"); ctx->Yield(30);`
      4. The snapshot shows `Active`, `!FetchInFlight`, `CommentCount >= 1`, and `FirstAuthor` equal to
         `"Ana Offline"`.
      5. Assert no comments fetch was attempted while down: `FetchIssueCommentsCalls()` did not grow.
         Reach the fake through the fixture-created backend only if an accessor exists; otherwise
         assert `CallsWhileDown() == 0`.
      6. Close the modal with the existing close helper, or by calling the `Close` path through a
         snapshot-driven `ImGui` escape. If no clean close exists, leave it open; the next test's
         `OpenCommentsModal` resets the state.

**Verify:** Linux `--test-case='CommentBlobFormat*,LazyEnrichment*,SmatchetCommentsModalSeed*,TicketSyncService*'`; CI `OfflineFirst`.
**Done when:** the cached thread shows offline, there is no stuck loading state, and the toast wording is truthful.

---

## S8 — feat(offline): pending-action queue; comments posted offline replay on reconnect `[visual]`

**Symptom for the Issue:** "Comments cannot be written offline — nothing is saved for the next
connection."

**Design (one generic queue, not one table per action — DRY).**

**Schema.** An additive SQLite pair in a NEW companion TU
`Source/Core/src/Persistence/LocalCacheManager_PendingActions.cpp`, called from `InitSchema()` right
after `InitLookupCacheSchema_();` via `InitPendingActionsSchema_();`:
- `pending_actions(id INTEGER PRIMARY KEY AUTOINCREMENT, backend_key TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL, issue_key TEXT NOT NULL, payload_json TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0, last_error TEXT, created_at INTEGER NOT NULL)`
- `pending_actions_dead(dead_id INTEGER PRIMARY KEY AUTOINCREMENT, original_id INTEGER NOT NULL, backend_key TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL, issue_key TEXT NOT NULL, payload_json TEXT NOT NULL, attempts INTEGER NOT NULL, last_error TEXT, created_at INTEGER NOT NULL, archived_at INTEGER NOT NULL, terminal_reason TEXT NOT NULL)`
- `CREATE INDEX IF NOT EXISTS idx_pending_actions_backend ON pending_actions(backend_key, created_at)`
- `state` is one of `pending`, `sending`, `ambiguous`, `needs_review`.

**Types.** NEW `Source/Core/include/Sync/PendingActionTypes.h`:
- `enum class PendingActionKind : unsigned char { CommentAdd, WorklogAdd, WatchAdd };`
- `const char* PendingActionKindWire(PendingActionKind)` returning `"comment_add"`, `"worklog_add"` or
  `"watch_add"`, and `bool ParsePendingActionKind(const std::string&, PendingActionKind&)`.
- `struct PendingActionRecord { std::int64_t Id = 0; std::string BackendKey; std::string Kind; std::string IssueKey; std::string PayloadJson; std::string State; int Attempts = 0; std::string LastError; std::int64_t CreatedAtEpochSec = 0; };`
- `struct DeadPendingAction` with the dead-table columns.

**`ISyncCache` / `LocalCacheManager` / `FakeSyncCache`.** Add these pure virtuals, implemented in the
new TU and in `FakeSyncCache`:
- `std::int64_t EnqueuePendingAction(const std::string& backendKey, const std::string& kind, const std::string& issueKey, const std::string& payloadJson)`
- `std::vector<PendingActionRecord> LoadPendingActions()`
- `void UpdatePendingAction(std::int64_t id, const std::string& state, int attempts, const std::string& lastError)`
- `void DeletePendingAction(std::int64_t id)`
- `void ArchivePendingAction(std::int64_t id, const std::string& terminalReason, const std::string& terminalError)`
- `std::vector<DeadPendingAction> LoadDeadPendingActions()`
- `bool RestoreDeadPendingAction(std::int64_t originalId)`
- `void DeleteDeadPendingAction(std::int64_t deadId)`

On startup (in the schema init), run `UPDATE pending_actions SET state='ambiguous' WHERE state='sending'`.

**Service.** NEW `Source/Core/include/Sync/PendingActionQueueService.h` +
`Source/Core/src/Sync/PendingActionQueueService.cpp`, a separate TU because `OfflineQueueService.cpp`
is 1507 lines. Deps: `IOfflineQueueDeps&`, plus a new `IOfflineQueueDeps::CollaborationShared()`
returning a latched `std::shared_ptr<ITrackerCollaboration>`. Implement it in the adapter as an
aliasing `shared_ptr` of the latched backend, like `ReaderShared`, and in `FakeOfflineQueueDeps`.
- **`SubmitOutcome SubmitOrQueue(PendingActionKind kind, const std::string& issueKey, const std::string& payloadJson, TrackerConnectivityState connectivityAtKick)`**
  (worker-safe): `RouteWrite(conn, true, readOnlyPref)`.
  - `Reject` → Failed.
  - `QueueImmediately` → enqueue, returns Queued.
  - `NetworkFirst` → `Dispatch(kind, payload)`:
    - Ok → Sent.
    - A retryable pre-send failure → enqueue, Queued.
    - A Transport operation-timeout on a POST → enqueue in state `ambiguous` with
      `last_error = "sent; response lost"`, returns Queued.
    - Other → Failed.
  - `SubmitOutcome { enum Kind { Sent, Queued, Failed } K; std::int64_t QueueId; std::string Error; }`.
- **`Tick()`:** gated exactly like `TickOfflineFieldEdits`, reusing the read-only, cache, in-flight,
  timer and backend-key filter patterns and `ScopeExit`. Add `PushReplayTimersForward` /
  `RestartReplayTimersNow` twins and call them from the same places `OfflineQueueService`'s are called
  (`GridContextDepsAdapter::PushReplayTimers` / `RestartReplayTimers`).
- **Per row:**
  - `ambiguous` + CommentAdd → fetch comments. If `CommentAlreadyPosted(fetched, body, createdAt)`
    (pure, in `Sync/PendingActionPolicyPure.h`: normalised CRLF/trailing whitespace, equal body,
    `CreatedAtSec >= createdAt - 300`) → delete the row and write a "dedup" audit entry. Else treat it
    as pending.
  - `ambiguous` + WorklogAdd → `needs_review` (never auto-resend).
  - Otherwise: mark `sending` (persist), dispatch, then:
    - Ok → delete + audit;
    - retryable → `pending` with attempts+1, archived at `OfflineQueueReplayPolicy::kMaxReplayAttempts`;
    - Transport post-send → `ambiguous`;
    - non-retryable → archive `replay_rejected`.
- **Audit:** `BackendAuditTrail::AppendResult("offline_replay_action", "offline_action_replay", issueKey, id, ok, err, {kind})`
  for every replay, `"offline_queue_action"` for every enqueue.
- **Owner:** AppController, via `std::unique_ptr<PendingActionQueueService> pendingActions_;` declared
  after `offlineQueue_`, constructed next to it in `AppController_Init.cpp`, and ticked next to
  `TickOfflineFieldEdits` at both call sites (`Ui/SmatchetUI.cpp` and `Ui/SmatchetImGuiHost.cpp`) via
  `AppController::TickPendingActions()`.
- **In-memory mirror:** `std::vector<PendingActionRecord> PendingSnapshot() const`, refreshed on the
  worker after each enqueue or tick. The UI reads only this.

**Comment kind.** Payload `{"body": "<text>", "created": <epoch sec>}`.
- `AppController::SubmitOrQueueComment(issueKey, body)` → `pendingActions_->SubmitOrQueue(CommentAdd, ...)`.
- The modal post path calls it (on the existing worker), replacing `AddIssueCommentPlain` in
  `SmatchetCommentsModalUi.cpp`:
  - Sent → "Comment Posted" toast + refetch.
  - Queued → the real `comments.queued_title` "Comment Queued" toast with the body
    `comments.queued_body` "Saved offline; it will post when the tracker is reachable." (new key), and
    clear the draft.
  - Failed → error toast, draft kept.
- Remove S7's offline-disabled state and the `post_offline_hint`.
- The modal thread appends the pending comments for this issue from `PendingSnapshot()` with a
  `(pending)` badge (`freshness.badge_refreshing` is fine; better, add `comments.pending_badge`
  "(waiting to sync)"). Dead ones show `comments.failed_badge` "(failed — see Offline Queue)".
- On a successful replay of a CommentAdd for the open modal's issue, the modal refetches (Gen-guarded)
  via a post-back.

**Offline queue UI.** NEW `Source/Core/src/Ui/SmatchetOfflineQueueUi_Actions.cpp`, called from
`DrawUnifiedOfflineQueuesPanel` after `DrawOfflineQueueTable(ctx)`:
- It draws a second small table "Other queued changes" (Kind, Issue, State, Retries, Last error,
  Created) with actions:
  - Retry now → `RestartReplayTimersNow` + Tick;
  - Discard (confirm);
  - Restore (dead);
  - for `needs_review` rows: "Send again" / "Discard".
- It uses a new narrow facet `Interfaces/IAppPendingActions.h` (rank 0) that AppController implements,
  so no new `AppController.h` includer is needed.
- Also fix "Retry creates now" (`DrawOfflineQueueToolbar`): it calls `RestartReplayTimersNow` before
  `TickOfflineCreates`/`TickOfflineFieldEdits`, so it works while timers are pushed forward. Add the
  AppController method `RetryOfflineQueuesNow()`.

**Status bar:** the queued count adds `PendingSnapshot().size()` (in memory).

**Docs:**
- `offline-sync.md`: mention `PendingActionQueueService` in the description.
- `Sync/AGENTS.md` first bullet: comments, worklogs and watch now enqueue via `PendingActionQueueService`.
- `code-review.md`: the Write bullet names it.
- `docs/plans/shipped/issue-comments.md` risk line "Comments bypass offline-queue" is left as history.
  Add an ADR-0026 "Consequences" line saying comments now queue.

**Tests:**
- `PendingActionsSqlite.test.cpp` (SmatchetTests): CRUD, archive/restore, the startup
  `sending`→`ambiguous` migration, and an old DB gaining the tables.
- `PendingActionPolicyPure.test.cpp` (both lists): `CommentAlreadyPosted` cases.
- `PendingActionQueueService.test.cpp` (TSan + SmatchetTests, fakes only):
  - offline → Queued with 0 posts;
  - online → Sent;
  - retryable → Queued;
  - replay → Ok deletes;
  - an ambiguous comment already on the server → deleted with exactly 0 POSTs;
  - an ambiguous comment not found → exactly 1 POST;
  - a 4xx archives;
  - the attempts cap archives.
- Bucket E `Comments_PostedOfflineReplays` (API-level, like S6):
  1. `TransportDown` (wait for the state), then call
     `app->SubmitOrQueueComment("OFF-1", "offline hello")` and assert the outcome is Queued.
  2. Assert `app->PendingActionsSnapshot().size() == 1`, using the `IAppPendingActions` accessor.
  3. Set `Up`, run `ConsumeTrackerConnectivityRecovery()` and `RetryOfflineQueuesNow()`, and yield until
     the snapshot is empty (≤ 600 frames).
  4. Assert `CallsWhileDown() == 0` for comment posts. The replay succeeded, so the queue emptied
     without a dead-letter row (`LoadDeadPendingActions` empty).

**Done when:** an offline comment survives a restart (the SQLite test) and posts exactly once after
reconnect.

---

## S9 — fix(offline): worklogs, watch and Annotate comments go through the pending-action queue

**Symptom for the Issue:** "Worklogs, 'Watch' and Annotate comments fail offline and are lost."

- **Worklog.**
  - Payload `{timeSpent, timeRemaining, adjustEstimate, description, started}`.
  - `HandleWorklogSave` calls `SubmitOrQueueWorklog` instead of `SubmitWorklog`. Keep the existing
    in-flight/Gen guards; Queued shows the "Worklog queued offline" toast.
  - Extract the worklog dialog, `TicketFieldEditor.cpp` ≈ lines 1107-1475 (everything from the worklog
    state struct through `HandleWorklogSave`), into NEW
    `Source/Core/src/TicketFieldEditor_Worklog.cpp` with a `TicketFieldEditor_Worklog.h`. Move it
    verbatim first in its own commit, then change it. That brings `TicketFieldEditor.cpp` to about
    1390 lines. The new TU must not include `AppController.h` if any `IApp*` facet suffices; otherwise
    STOP and report, since the fan-in gate would fail.
- **Watch.** `TrackerGridFieldDisplay.cpp` "Watch" calls `SubmitOrQueueWatch`. Queued counts as success
  for the button (it hides) with the tooltip "Queued — will apply when the tracker is reachable".
- **Annotate.**
  - `AddIssueCommentPlain` (quick template) and `AddIssueCommentAnnotateContext` go through
    `SubmitOrQueueComment`.
  - For the annotate-context text, build the body with the same helper the backend uses. Find it with
    grep; if the text is built backend-side, add a pure builder in `Tracker/` and use it in both
    places. Do not duplicate it.
- **Commands.** `ticket.add_comment` and `ticket.add_worklog` return
  `{"ok":true}` / `{"queued":true,"offlineId":<id>}`.
- **Move the four direct `Collaboration()->Add*` writes** out of `AppController_CatalogAndFieldEdit.cpp`
  into `PendingActionQueueService::Dispatch`. The AppController methods become thin wrappers.
- Then flip `offline-write-bypasses-queue` to **absolute-0**:
  - in `test-lint-rules.sh`, replace the delta call for that rule with
    `compute_offline_exact_violations | grep -F $'offline-write-bypasses-queue\t'`;
  - add a bats test asserting the whole-tree scan is empty for that rule;
  - update the AGENTS.md contract row wording ("first blocking absolute-0, second delta-gated") without
    adding lines;
  - update ADR-0026's graduation note.
- **Tests:**
  - `PendingActionQueueService` cases for worklog (ambiguous → `needs_review`) and watch (idempotent
    retry);
  - `BuiltinCommandsDispatch` envelope cases for `queued:true`;
  - bucket E `Worklog_OfflineQueues`.

---

## S10 — fix(offline): command/MCP/Lua/Annotate field edits, sprint/estimate and bulk import queue offline

**Symptom for the Issue:** "Field edits made from commands, MCP, Lua or Annotate fail offline; sprint
and estimate edits cannot be queued; bulk import loses rows offline."

- **Commit-or-queue on the facet.** Add to `Interfaces/IAppTicketMutations.h`:
  `virtual FieldEditCommitOutcome SubmitFieldEditOrQueue(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values) = 0;`.
  Forward-declare the outcome type if the header rank rules require it.
  - The AppController implementation builds a `FieldEditCommitRequest` and captures the base like the
    grid does: `OriginalValue = ticket.GetFieldValue(field.Id)`, `HasOriginalValue = true`, and
    `OriginalRichValue` for rich fields. Capturing the base is what lets the replay conflict gate
    re-fetch and fill Plane's key map.
  - It then calls `CommitOrQueue` synchronously (callers are already on workers or command threads)
    and, on SavedOnline/QueuedOffline, applies `ApplyFieldEditResult` via `RunOnUiThread`.
  - `SubmitFieldEdit` keeps its signature and becomes a wrapper returning
    `VoidOk()` for Saved/Queued, else `Err`.
- **Callers:**
  - `BuiltinCommands_TicketMutations.cpp` (`ticket.set_field`, `ticket.transition`, `ticket.set_fields`):
    call `SubmitFieldEditOrQueue` and add `"queued":true` to the success envelope when queued.
  - `AppController_LuaBindingsCore.cpp` `TicketSetFieldGlue`/`TicketTransitionGlue`: return
    `(true, "queued")` when queued (Lua `ok, err`).
  - `Ui/AnnotateAnalysisUi_Window.cpp` (both `SubmitFieldEdit` calls): a "Queued offline" status line
    when queued.
- **Sprint and estimate queueable.**
  - Remove the two early `return false;` exclusions in `FieldEditSupportsOfflineQueue` for sprint and
    for editable estimates. Keep the non-editable timetracking exclusion.
  - The prepared payload for estimates is `{"timetracking": {...}}`, built the same way
    `SubmitTimetrackingFieldEditNetworkOnly` builds it; extract a shared helper, no duplication.
  - For sprint, `{"sprint_add": "<sprintId>"}`, and add to `OfflineQueueService::ReplayOneFieldEdit`,
    before `UpdateIssueFields`: if the payload has exactly the key `sprint_add`, call
    `mutations->AddIssueToSprint(row.IssueKey, id)` instead.
  - Add replay tests for both.
- **Bulk import.**
  - Extract `QueueNewIssueDraftOffline`'s queue call into NEW
    `Source/Core/include/Ui/IssueCreateOrQueue.h` + `src/Ui/IssueCreateOrQueue.cpp`, taking
    `IAppTicketMutations&`:
    `IssueCreateOrQueueOutcome CreateOrQueueFallback(IAppTicketMutations&, const IssueDraft&, const IssueCreateResult& failed)`,
    which queues when `failed.ErrorTransient`.
  - Use it in `SmatchetNewIssueDraftUi.cpp` (behaviour unchanged) and `SmatchetBulkTicketsUi.cpp`:
    status `"queued"`, which is already treated as non-terminal, then `"queued offline #<id>"`.
  - Do not queue the "created, key unknown" shape (the existing `ErrorTransient=false` contract).
- **Tests:**
  - `FieldEditPipelineService` sprint/estimate queue + replay;
  - `OfflineQueueServiceRuntime` `sprint_add` replay;
  - command envelopes;
  - a Lua binding test if a Lua host fixture exists (`tests/support/LuaHostFixture.h`);
  - a bulk-import pure test for the status strings.

---

## S11 — fix(offline): persist component options, users and edit permissions for offline use `[visual]`

**Symptom for the Issue:** "Offline, the components editor spins on 'Loading components…' forever,
component cells show raw ids, user suggestions are empty, and edit permissions refetch every frame."

- **Components.**
  - `lookup_cache` kind `project_components`, key = the project key, payload = the options JSON
    (reuse `LearnedWorkflowPure`'s option (de)serializer; rename it to
    `SerializeFieldOptions`/`ParseFieldOptions` in a shared `FieldOptionsJsonPure.{h,cpp}`, with S5's
    functions becoming thin wrappers).
  - `EnsureProjectComponentsLoaded` and `EditMetaCacheService::WarmIssueTypeEditMetaWorker`:
    - check `IsOfflineState` first;
    - on success, upsert to the store on the worker;
    - on first use per backend, bulk-load stored rows on a worker into `projectComponentOptions_`
      (seed only; never overwrite live).
  - `TicketFieldEditor.cpp` components: replace `"Loading components\xE2\x80\xA6"` with
    `DataFreshnessCue::Draw` of the classified state. In `UnavailableNoCache` fall back to
    `field.AllowedValueOptions` (the catalog's components), so offline component edits work.
  - Fix the stale doc comment at `AppController.h` (`GetComponentOptionsForProject` "caller falls
    back…") to describe the new fallback.
- **Users.** Kind `users`, key `all`, payload `[{accountId, displayName, email}]`, saved on the worker
  after a successful catalog fetch, loaded at `InitFieldCatalog` on a worker. Replace
  `AppController_PaneContexts.cpp:884`'s context-retirement `AvailableUsers` swap: leave it, it is
  legitimate, and add a deviation marker for `offline-cache-cleared`.
- **Editmeta.** Kind `editmeta_type`, key `issueTypeKey`, payload `{fieldId: canEdit}`. Seed
  `issueTypeEditMeta_` from the store at startup warm-up; save after each successful per-type load.
- **Tests:**
  - `FieldOptionsJsonPure`;
  - `EditMetaCacheService` (store seeds and upserts, via `FakeLookupCache`);
  - a component persistence round trip (a fake-deps test in the style of `EditMetaCacheService.test.cpp`);
  - bucket E `Components_OfflineShowsSavedOptions`.

---

## S12 — fix(offline): project picker, Views Fields tab, Annotate lookup and pane catalog work offline `[visual]`

- **Projects.**
  - Add `virtual Result<std::vector<RemoteProject>, TrackerError> ListProjectsTyped()` to
    `ITrackerConnectivity`. Its default wraps `ListProjects()` as Ok.
  - Jira, Plane, GitHub and Linear override it, returning classified errors where they return `{}`
    today (`ClassifyRejectedHttpStatus` with the status in hand; Parse for parse failures).
    `ListProjects()` stays as a wrapper.
  - Kind `projects`, key `all`.
  - `SmatchetProjectPicker.cpp` uses `ListProjectsTyped`:
    - on success, persist;
    - on failure, load stored → render with `DataFreshnessCue`;
    - `fetchError` is set from the typed error, so Retry appears;
    - "No projects found." only when `Fresh` and empty.
- **Views Fields tab** (`SmatchetViewsDashboardUi.cpp`): while `d.fieldCatalogLoading`, render the
  cached `availableFields` (not "Refreshing field catalog...") with `DataFreshnessCue::Draw(Refreshing)`,
  and do not disable editing when fields exist. When empty and `GetFieldCatalogError()` is non-empty,
  show the error plus a "Retry" that sets `d.triggerCatalogRefetch = true`.
- **Annotate lookup** (`AnnotateAnalysisUi_Modals.cpp`):
  - thread the `TrackerError` kind from `SearchUsersByQuery`: add a typed variant on `IAppUsers` if
    needed, mirroring `FetchIssueCommentsTyped`;
  - on Transport, show `annotate.user_unknown_offline` "Unknown (offline)" instead of "Past Employee",
    and keep assign disabled with that reason;
  - fall back to `GetAvailableUsers()` (now persisted by S11) by matching the p4 user to
    email/displayName.
- **Pane catalog** (`AppController_PaneContexts.cpp populatePaneCatalogAfterSync_`): on fetch failure,
  load the `FieldCatalogCache` snapshot on the worker and post it through the same generation-checked
  apply. Keep the non-fatal log.
- **Tests:**
  - `ProjectPicker` typed-error cases for each client (HTTP fixtures that already exist);
  - an Annotate offline pure test if the logic is extracted (extract `ClassifyUserLookupOutcome` to a
    pure header);
  - bucket E `ProjectPicker_OfflineShowsSavedProjects`.

---

## S13 — fix(offline): attachment disk cache, watchers/votes and User Info offline states `[visual]`

- **Attachments.**
  - `DownloadAttachmentToLocalFile` routes through `TrackerHttpClient` instead of the direct
    `cpr::Get`, keeping the host allowlist, size cap and redirect check, and removing the deviation.
  - It writes into a content cache at `ConfigManager::GetUserDataDirectory() + "attachment_cache/<backendKey>/<sha of url>/<sanitized filename>"`:
    - use `ConfinePathUnderSubdir`;
    - write through `ConfigManager::AtomicWriteTextFile` or an equivalent binary temp-then-rename;
    - cap the cache at 256 MB with LRU by mtime, pruned on a worker after each download.
  - A cache hit skips the network entirely, so preview and open work offline for previously viewed
    attachments.
  - The preview error for a miss while offline reads
    `attachments.unavailable_offline` "Not downloaded yet — available once online".
- **Watchers/votes** (`Tracker/TrackerGridFieldDisplay.cpp`, which can include the root
  `DataFreshnessCue.h`):
  - on load failure with a Transport kind, show
    `DataFreshnessCue::Draw(UnavailableNoCache, err)` and "Load" as retry;
  - cache the last successful list per issue in memory for the session.
- **User Info** (`Ui/SmatchetUserInfoUi_Sections.cpp`):
  - add a Retry button for the groups and members errors;
  - record the error in `pollMembersFuture` when the worker throws, which stops the per-frame relaunch.
- **Calibration review.**
  - Run `--scan-offline`.
  - For each WARN rule, record hits and false positives in `docs/high-integrity/offline-calibration.md`.
  - Propose graduations in the PR body (no flip without the ADR-0026 criteria).
- **Tests:**
  - `AttachmentCachePure` (path building, LRU selection);
  - an attachment cache-hit test with a fake downloader seam (extract the download function behind a
    small interface);
  - bucket E `Attachment_OfflineOpensCachedFile` if feasible; otherwise note it as a residual in the PR.

---

## Approach

Thirteen dependency-ordered slices, each one PR (see § Slice index). S1 fixes the worst data-loss path
first; S2–S3 land the shared primitives and the offline test harness; S4 lands Pillar 6 and its gates
so every later slice is gated; S5–S7 fix the two reported regressions; S8–S13 finish the write queue
and the persisted read lookups.

## Files to modify

Listed per slice in each slice's **Files** table (authoritative for that slice's scope — contract §4).

## Existing utilities reused

`ClassifyRejectedHttpStatus` / `TrackerErrorFromHttpStatus` / `TrackerErrorParse`
(`Tracker/TrackerError.h`); `FieldCatalogCache` snapshot save/load; `OfflineQueueService` replay +
dead-letter + `OfflineQueueReplayPolicy`; `BackendAuditTrail`; `json_safe::ParseBounded`
(`Json/BoundedJsonParse.h`); the `ScopeExit` class (moved, S2); `FakeTrackerClient` +
`JiraFakeTrackerFixture` + `ScriptedTrackerBackendFactory`; `scripts/dev/test-ui-jira-deterministic-backend.sh`
(wrapped, S3); `QueueCreateOffline` (S10 bulk import); the `debug.grid.edit-burst` pipeline entry
`ProcessGridFieldEdits` (S6 bucket-E).

## Extraction sizing

S9 moves the worklog dialog (≈ 370 lines) out of `TicketFieldEditor.cpp` (1760 lines) into
`TicketFieldEditor_Worklog.cpp`, verbatim first, in its own commit. S5/S8 add companion TUs
(`LocalCacheManager_Lookup.cpp`, `LocalCacheManager_PendingActions.cpp`) instead of growing
`LocalCacheManager.cpp` (1402 lines). Every other slice is additive and stays under the size caps.

## UX Pillar callouts

- **Pillar 1 (perf):** status-combo work runs only while the combo is open; the cue strings are static.
- **Pillar 2 (no freeze):** all new SQLite I/O runs on workers. S6 moves the field-edit enqueue off the
  UI thread; S5 moves the learned-transition I/O off it.
- **Pillar 3 (no crash):** `RunKeyedFetch` / `ScopeExit` make stuck in-flight latches impossible;
  `lastState_` becomes atomic.
- **Pillar 4 (a11y):** no change. The cues are text, so they are readable at any font scale.
- **Pillar 6 (offline-first):** introduced by this plan (S4).

## Perf-review-system gates

This plan touches `Source/Core/`.
1. **PR-fast CI.** Standard, per slice.
2. **Pillar 2 static scanner.** Run `bash scripts/dev/pillar2-scan.sh` on every touched `.cpp`. New
   SQLite work goes only to workers: `lookup_cache` I/O (S5, S11), queue inserts (S6, S8) and the
   learned-row bulk load (S5).
3. **Dispatcher drain.** Post-backs stay small; comment seeds are parsed on the worker (S7).
4. **Visible-cue bucket-E harness.** The `OfflineFirst` lane (S3+) asserts the cues.
5. **Marker inventory.** The per-frame `Ensure`/`Get` for status transitions runs only while the combo
   is open. `GetAvailableTransitions` is a mutex plus two hash lookups; the cue strings are static.

## Risks / non-goals

- **Cold start.** The state is `Unknown` until the first probe, so the first offline edit goes
  network-first. `RequestTrackerProbeNow` (S6/S7) pulls the probe forward.
- **Learned transitions can be stale.** Jira conditions or workflow edits may make a remembered move
  invalid; replay then dead-letters it and it can be restored from the queue panel. A fetched list
  containing the from-status is never learned (S5).
- **Comment dedupe.** Posting the same body twice within 5 minutes on purpose is treated as a dup, which
  is accepted. A worklog in the ambiguous state always asks the user (S8/S9).
- **Plane after restart.** Replay depends on the conflict re-fetch filling the key→UUID map. S10 makes
  every command edit capture a base, so the re-fetch runs.
- **Non-goals:** editing or deleting existing comments offline, and offline issue search.

## Verification

- **Doc validation (blocks plan-doc PRs — keep this bullet):** `bash agents/scripts/core/test-markdown-links.sh`, `bash scripts/dev/test-markdown-lang-tag.sh`.
- **Plan stress-test — `grill-with-docs` (keep this bullet):** replaced for this plan by two independent code-verification rounds (six read-only agents) that re-checked every anchor, signature and claim against `5b4b1c3`; each slice's contract §3 STOP rule catches drift since.
- **Linux:**
  - `bash scripts/dev/pre-ship.sh origin/develop`
  - `bash agents/scripts/project/test-lint-rules.sh --selftest`
  - bats
  - `posix-core-check`
  - `ninja-test-linux` → `SmatchetTsanTests`, all green
- **CI (Windows):** `SmatchetTests`, ASan/UBSan, and bucket-E `JiraDeterministic` + `OfflineFirst`, all
  green.
- **Manual:**
  1. Block the tracker host in the hosts file, then start the app. The grid, catalog, status combo
     (saved workflow), comments, components and project picker all show saved data with cues.
  2. Change a status, post a comment and log work. Each shows "Queued offline", and the Offline Queue
     panel lists them.
  3. Restart while still offline: the queue is intact.
  4. Unblock the host. Everything replays once, the queue empties, and the audit trail shows the
     replays.

## Implementation log

(each slice appends here after merge)

### S1 — [#2238](https://github.com/alexandrosk0/Smatchet/pull/2238)
- Shipped: Jira / Linear catalog failures keep their `TrackerError` kind (the #21b collapse is gone); `CatalogOfflinePolicyPure.h`; `HandleFieldCatalogError` never clears the catalog and restores the snapshot whatever the error kind; the grid holds pending edits under a banner-driven read-only state; a failed catalog fetch no longer wipes users; the startup "Working offline:" banner is recognised.
- Tests: `CatalogOfflinePolicyPure` (new), `TrackerCatalogBuild` (500 → ServerError, 401 → Auth, unreachable → Transport), `ConnectivityMonitorService` (startup wording).
- Review fix (CodeRabbit): the offline catalog banner names the configured backend (Jira / Plane / GitHub / Linear); it previously said "Jira" for every non-Plane backend. `HandleFieldCatalogError` now takes the normalized backend key instead of a `catalogPlane` flag.

### S2 — [#2240](https://github.com/alexandrosk0/Smatchet/pull/2240)
- Shipped: `ConnectivityMonitorService` shared connectivity tracking; `KeyedLookupCache` pattern for caching keyed operations with TTL and freshness cue; offline metadata persistence for UI.
- Tests: `KeyedLookupCache` (new), `ConnectivityMonitor` (transient flag handling).
- Deviations: follow-up work on tracking offline state transitions and caching strategies.

### S3 — [#2245](https://github.com/alexandrosk0/Smatchet/pull/2245)
- Shipped: `FakeNetworkSwitch` (process-wide atomic network mode); `FakeTrackerClient` network gating on all network-shaped calls; `JiraFakeTrackerFixture` parsing of `"network"`, `"catalog.fields"`, `"transitions"`, `"comments"` JSON keys; `offline-first.json` fixture with OFF-1/OFF-2 tickets; `offline_first.test.cpp` bucket-E UI tests; `test-ui-offline-first.sh` driver script; CI bucket-e-offline-first lane with 2-attempt Mesa GL retry.
- Tests: 5 new JiraFakeTrackerFixture doctests (network mode, field catalog, transitions, comments, backward compatibility).
- Verification: offline-first.json valid JSON; lint gates pass; placeholder UI tests ready for network simulation hook (S4).
### S2 — [#2240](https://github.com/alexandrosk0/Smatchet/pull/2240) (stacked on #2238)
- Shipped: `OfflineFirstPure.h` (IsOfflineState / ShouldAttemptNetwork / ClassifyFreshness / ShouldRenderContent / RouteWrite), `KeyedLookupCache.h` + `RunKeyedFetch`, shared `ScopeExit.h` (moved out of `OfflineQueueService.cpp`), `DataFreshnessCue` + 7 `freshness.*` strings, atomic `ConnectivityMonitorService::lastState_` + `RequestProbeNow` / `AppController::RequestTrackerProbeNow`, `IAppSync::IsTrackerOffline()`, `TrackerConnectivity()` on `IEditMetaDeps` / `IFieldEditDeps`.
- Tests: `OfflineFirstPure`, `KeyedLookupCache` (both lists); the concurrency case runs 8 threads × 1000 `TryBeginFetch` calls.
- Nothing consumes the primitives yet (S5+ do); `DataFreshnessCue` is compiled but unused.

## Deviations from plan

- **S2 (CodeRabbit review on #2240):** these override the S2 code blocks above; S5+ read the headers, not the plan.
  - `ClassifyFreshness` returns `Fresh` only while the tracker is reachable. This session's live data reads `CachedOffline` once the tracker drops.
  - `RunKeyedFetch` marks the outcome recorded only after `CompleteSuccess` / `CompleteFailure` returns, so a throwing completion also clears `InFlight`.
  - The cue texts are state-neutral: `freshness.cached_stale` is "Showing saved data" (the failure detail goes in the tooltip), and `freshness.unavailable_offline` was renamed `freshness.unavailable` ("Not available yet"). Neither state implies a failure or an outage it can't know about.

## Verification (actual)

## Archive
