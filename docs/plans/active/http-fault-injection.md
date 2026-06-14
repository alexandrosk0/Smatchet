# Plan — HTTP fault-injection tests (testing-surface Slice D)

> **Slug**: `http-fault-injection` (matches this file's basename without `.md`).
>
> **Status**: `active`.
>
> **Parent**: [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **D** (§6 P1 Gap 2; `debt.md:65`). Part of the approved additive block H→A→D→E1. Test-only slice — no product code.

## Context

`testing-surface.md` §5 Gap 2 says transport faults (429 / 500 / partial-page / retry / pagination) are untested, and §5.1 row 2 assumes the only way to reach them is a new `IHttpTransport` seam. The roadmap recon (#2) pushed back: `JiraCatalogHttpFixture.h` is an **in-process httplib loopback server** that the real `JiraClient` drives over real cpr, so most transport faults are reachable with no new seam.

Slice-D recon **corrected the roadmap's recon #2** on one load-bearing point. The claim "`TrackerHttpRequestWithRetry` retry … is already exercised" is **false**:

- **`TrackerHttpRequestWithRetry` has zero production callers** (grep-confirmed across `Source/`; not a stale-index artifact). Its header migration plan (Phase 2B/2C → migrate `JiraClient` / `JiraIssueSearch`) never landed. It is **dead code**.
- **Jira and Plane issue all HTTP via the no-retry `TrackerXxxLogged` helpers** (`TrackerGetLogged` / `TrackerPostLogged` / `TrackerPutLogged` / `TrackerPatchLogged` in `TrackerHttpUtils.h`). No retry, no backoff, no 429/5xx re-attempt anywhere on the live path.
- **`ClassifyTrackerResponse`** is used only by `PlaneIssueSearch.cpp`, and even there with no retry loop.

So the retry wrapper can only be tested as a **pure unit** (drive its `requestFn` lambda directly); it cannot be reached through the fixture. Pagination (`startAt` loops in `JiraIssueSearch.cpp`) **is** real-cpr-exercised through the fixture, so the recon's pagination half stands. Forcing 429/500 against a live Jira path therefore characterizes **graceful degradation with no retry** (the current truth), not retry/backoff.

Second recon correction: **the fixture serves GET only** (`server_.Get(".*")`, `JiraCatalogHttpFixture.h:43`) — POST/PUT/PATCH fall through to 404, so no mutation endpoint is testable today.

**Intended outcome after this lands**: the retry wrapper's contract is pinned by pure-unit tests; Jira pagination assembly + no-retry degradation on 429/500 are pinned by loopback-integration tests; the fixture serves mutations; and the dead-retry-wrapper gap is documented with a backlog follow-up instead of silently implied-tested.

## Approach

Three test surfaces + one fixture extension + doc-corrections, all test-only (`tests/`), no `Source/Core/` change.

1. **Retry wrapper — pure unit** (`tests/Core/TrackerHttpRetry.test.cpp`, new). Drive `TrackerHttpRequestWithRetry(requestFn, maxAttempts, cancelled)` with a lambda returning scripted `TrackerHttpResult`s and a captured call counter. Assert retry **count** and terminal **error kind** — not wall-clock backoff (the backoff is a real `std::this_thread::sleep_for`, not injectable; timing assertions would flake). Also unit-test `ClassifyTrackerResponse` over synthetic `cpr::Response` status codes. Sleep budget is bounded by capping `maxAttempts ≤ 3` (≤ 250+500 = 750 ms per worst-case case; whole-file budget < 1.5 s).

2. **Fixture extension** (`tests/support/JiraCatalogHttpFixture.h`). Register `server_.Post/Put/Patch(".*")` → `Dispatch`, and key the scripting maps on `method + " " + path` so a GET-then-POST on the same path (the transitions endpoint) can be scripted independently. Keep the existing GET-only call sites working by defaulting the new `method` parameter to `"GET"` on `ScriptJson` / `ScriptHandler` / `ScriptStatus` / `RequestCount`. Additive, back-compatible.

3. **Pagination + degradation — loopback integration** (`tests/Core/JiraHttpFaults.test.cpp`, new). Real `JiraClient` against the fixture: assert `startAt` page assembly (search-JQL + comment loops) reaches every page and stops, via `ScriptHandler` branching on the page cursor and `RequestCount`; assert forced 429/500 on a search page degrades **without** retry (characterization — exactly one request to the failing path, error surfaced). Mutation happy-path + error-path (transitions POST, field PUT, comment POST) once the fixture serves them.

The retry wrapper being dead code is **flagged, not fixed here** — wiring it into the live Jira/Plane paths is a product behaviour change (perf-gate + careful review) and gets its own backlog entry / deferred slice, not Slice-D scope creep.

Non-obvious trade-off: testing the retry wrapper as a pure unit while *also* characterizing that production does **not** retry looks contradictory but is the honest state — the unit test pins the wrapper's contract for when it's finally wired; the integration test pins today's no-retry reality so the future wiring slice has a red-to-green signal.

## Files to modify

Test-only. None under `Source/`.

1. `tests/support/JiraCatalogHttpFixture.h` — add Post/Put/Patch route registration + method-keyed scripting (default `"GET"` for back-compat). ~30 lines.
2. `tests/Core/TrackerHttpRetry.test.cpp` (new) — pure-unit tests for `TrackerHttpRequestWithRetry` + `ClassifyTrackerResponse`. No fixture, no HTTP.
3. `tests/Core/JiraHttpFaults.test.cpp` (new) — loopback-integration: pagination assembly, 429/500 no-retry degradation, mutation verbs.
4. `tests/CMakeLists.txt` — register the two new `.test.cpp` TUs in the doctest target.
5. `docs/guides/testing-surface.md` — §5 Gap 2 + §5.1 row 2 doc-corrections (see § Doc-corrections).
6. `docs/plans/active/testing-surface-roadmap.md` — fold corrections: recon #2 (retry not exercised — pure-unit only), Slice A row (already shipped via #1180), Slice D row status.

**Grep-before-naming check**: `rg -l 'TrackerHttpRetry|JiraHttpFaults' tests/` confirmed neither TU exists. `ClassifyTrackerResponse` / `TrackerHttpRequestWithRetry` live only in `Source/Core/{include,src}/Tracker/TrackerHttpClient.{h,cpp}`.

## Existing utilities reused

- `JiraCatalogHttpFixture` + `ScriptJson` / `ScriptHandler` / `ScriptStatus` / `RequestCount` / `Config` — `tests/support/JiraCatalogHttpFixture.h:36` — the loopback server; extended (not replaced) for mutations.
- `TrackerHttpRequestWithRetry` / `ClassifyTrackerResponse` / `TrackerHttpResult` — `Source/Core/include/Tracker/TrackerHttpClient.h:40,48` — the units under test.
- `TrackerErrorFromHttpStatus` / `TrackerError::IsRetryable` / `IsOk` / `Kind` — `Source/Core/include/Tracker/TrackerError.h` — to assert classification (429→RateLimited, 5xx→ServerError, 401/403→Auth, 404→NotFound, other-4xx→InvalidRequest, ≤0→Transport).
- doctest conventions (characterization style, `FindField` helpers, `BaselineFieldList`) — `tests/Core/TrackerCatalogBuild.test.cpp:1` — the template these new TUs follow.
- `JiraClient::FetchIssuesStreamed` (`JiraClient.h:121`) — the public search entry the pagination integration test drives (exact signature + how JQL/config thread in confirmed at implementation-read time).

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — test-only; no `Source/Core/` change, no UI-thread path touched.
- **Pillar 2 (UI never blocks > 100 ms)**: no impact — tests run on the ctest thread; the retry-unit tests deliberately pay real backoff sleep but never on a UI thread.
- **Pillar 3 (never crash)**: positive — pins crash-relevant edges (empty `requestFn` guard, `maxAttempts<1` clamp, 429/500 degradation, truncated-page handling).
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates

**N/A — diff does not touch `Source/Core/`.** Test-only (`tests/`, `docs/`). No scenario, no marker, no dispatcher path. If the deferred wiring slice (below) lands, *that* slice carries the perf-gate section.

## Risks / non-goals

- **Real-sleep backoff in retry-unit tests inflates ctest time.** Mitigation: cap `maxAttempts ≤ 3`, assert count+kind not timing, whole-file budget < 1.5 s. Accepted.
- **Cancel-during-backoff timing is coarse.** The cancel poll fires every 50 ms; the test asserts the loop *returns Cancelled with a low call count*, not an exact elapsed time. Accepted (no injectable clock).
- **Fixture method-keying could break existing GET callers.** Mitigation: new `method` param defaults to `"GET"`; existing `ScriptJson(path, body)` calls compile and behave unchanged. Verified by the existing `TrackerCatalogBuild.test.cpp` suite staying green.
- **Search/comment pagination cursor mechanism (startAt vs nextPageToken).** `JiraIssueSearch.cpp` comment loop uses `startAt += reportedMaxResults`; the search-JQL loop (`/rest/api/3/search/jql`) cursor is confirmed at implementation-read time before the assertion matrix is finalized. Risk: low — the test scripts whatever the client sends and asserts page count via `RequestCount`.
- **Non-goal — wiring the retry wrapper into production.** Explicitly deferred (see § Out of scope). Slice D only *tests* the wrapper + *characterizes* today's no-retry reality.
- **Non-goal — timeout / SSL / truncated-body faults (D2).** Need a real transport seam; httplib loopback can't synthesize them cleanly. Flagged, not built.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `TrackerHttpRetry.test.cpp` — retry count, error-kind classification, cancel paths, empty-`requestFn` / `maxAttempts<1` guards. `JiraHttpFaults.test.cpp` — pagination page-count assembly, 429/500 single-request no-retry degradation, mutation verbs. Both run under `ctest` via the doctest target.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Bash-driver scenario / screenshot / sanitizer**: N/A for new logic; the new TUs do compile under the existing `ninja-clang-asan` job, which exercises them under ASan (catches fixture thread/lifetime UB).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target) + `cmake --build --preset ninja-test-msvc` then `ctest` for the new TUs.
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs`**: run before finalizing; sharpen the "retry tested as unit vs characterized as no-retry in integration" framing against the domain model. Outcome recorded here pre-PR.
- **Manual residue**: none expected — every assertion is ctest-automated.

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray "retry already exercised" / "TrackerHttpRequestWithRetry wired" claims and correct them in this slice's PR (the roadmap recon #2 correction is the known one).

- **Wiring `TrackerHttpRequestWithRetry` into the live Jira/Plane paths** — the wrapper is dead code; making 429/5xx retry uniform is a product behaviour change (perf-gate, audit-trail, cancel-plumbing review). **Follow-up**: new backlog entry in `docs/self-improvement/categories/debt.md` ("wire retry wrapper into Jira/Plane GET+mutation sites, or delete it as dead code") — proposed as a deferred Slice **D3**. Slice D's integration tests give that slice its red→green signal.
- **D2 — timeout / SSL-handshake-failure / truncated-body faults** — need a real `IHttpTransport` seam (httplib loopback can't cleanly synthesize them). Deferred sub-slice, flagged in the roadmap.
- **Plane mutation/pagination faults** — this slice characterizes Jira; the Plane loopback fixture (if any) is a separate follow-up. `ClassifyTrackerResponse` (Plane-only consumer) *is* unit-tested here, so Plane's classification contract is covered even though its endpoints aren't scripted.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status to `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
