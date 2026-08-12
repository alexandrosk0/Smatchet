# Plan — HTTP fault-injection + retry wiring (testing-surface Slice D + D3)
<!-- plan-date: 2026-06-14 -->

> **Slug**: `http-fault-injection` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.
>
> **Parent**: [`testing-surface-roadmap.md`](testing-surface-roadmap.md) Slice **D** (§6 P1 Gap 2; `debt.md:65`) + the **D3** wiring follow-up (user-approved to do now). Part of the approved additive block H→A→D→E1. **Product change** — touches the strict zone `Source/Core/src/Tracker/`, so the perf-gate section fires.

## Context

`testing-surface.md` §5 Gap 2 says transport faults (429 / 500 / partial-page / retry / pagination) are untested, and §5.1 row 2 assumes the only path to them is a new `IHttpTransport` seam. The roadmap recon (#2) pushed back: `JiraCatalogHttpFixture.h` is an **in-process httplib loopback server** that the real `JiraClient` drives over real cpr, so most transport faults are reachable with no new seam.

Slice-D recon **corrected recon #2** on one load-bearing point:

- **`TrackerHttpRequestWithRetry` has zero production callers** (grep-confirmed across `Source/`; not a stale-index artifact). Its header migration plan (Phase 2B/2C) never landed → it is **dead code**.
- **Jira and Plane issue all HTTP via the no-retry `TrackerXxxLogged` helpers** (`TrackerGetLogged` / `TrackerPostLogged` / `TrackerPutLogged` / `TrackerPatchLogged` in `TrackerHttpUtils.cpp`). No retry, no backoff, no 429/5xx re-attempt anywhere on the live path.
- **`ClassifyTrackerResponse`** is used only by `PlaneIssueSearch.cpp`, with no retry loop.

So today a single transient 429/5xx fails a catalog fetch or issue search outright. The user approved **wiring the dead retry wrapper live now** (Slice D3) with a **safe-tiered** policy (chosen via review): retry idempotent verbs fully, retry non-idempotent POSTs only on pre-send transport failure.

**Intended outcome after this lands**: tracker HTTP transparently retries transient failures (Transport / 429 / 5xx) on idempotent verbs with exponential backoff; non-idempotent mutations (comment / worklog) never double-fire; the retry contract is pinned by pure-unit tests; and pagination + retry behaviour is pinned by loopback-integration tests.

## Approach

**Wire retry at the helper layer, not at ~45 call sites.** The `TrackerXxxLogged` helpers in `TrackerHttpUtils.cpp` are the single chokepoint every Jira/Plane/GitHub HTTP call already funnels through (each does `cpr::Verb(...)` → `NetworkUsageTracker::Record` → `LogTrackerHttpResult` → return). Slotting the retry loop *inside* those ~4 functions gives every call site retry with near-zero call-site churn, makes `TrackerHttpRequestWithRetry` live (reused internally), and keeps the diff small + reviewable. This is strictly better than the roadmap's implied per-site sweep and achieves its stated "retry on 429/5xx uniform across the codebase" goal at one seam.

Each helper's per-attempt body becomes the `requestFn` lambda: `cpr::Verb(...)` → `Record` → `Log` → `ClassifyTrackerResponse(resp)`. The wrapper retries that lambda, and the helper returns `.Response` (so callers still get a raw `cpr::Response` and their existing `status_code` / `text` checks are unchanged). Per-attempt `Record` + `Log` stay accurate (each real network attempt is accounted + logged; the wrapper additionally `LOG_DEBUG`s each retry).

**Safe-tiered retry policy** (the user-approved fork):

| Verb | Retries on | Rationale |
|---|---|---|
| GET (normal timeouts) | Transport (≤0) + 429 + 5xx | idempotent — re-read is free |
| PUT | Transport + 429 + 5xx | idempotent — field set is convergent |
| PATCH | Transport + 429 + 5xx | idempotent — Plane field set |
| POST | **Transport only** | non-idempotent (comment / worklog) — a post-send 5xx may mean the server *did* process it; retrying would double-fire |
| GET (probe overload, short timeouts) | **never** (maxAttempts=1) | preserves fast-fail connectivity probes — retry would defeat the 2 s/5 s probe budget |

Transport-only POST retry is safe for *all* POST shapes: if the request never landed (connection refused / DNS / no response, status ≤ 0) re-sending is harmless; if it landed and the server returned 5xx, we do **not** retry, so no duplicate. GitHub inherits retry for free via the shared helpers — also safe (its GETs are idempotent, its POSTs go Transport-only).

**Cancel plumbing**: add an optional `const std::function<bool()>& cancelled = nullptr` param to the helpers so the wrapper can poll it every 50 ms during backoff. Thread the existing `shouldCancel` token into the Jira/Plane **search-fetch GET loops** (`JiraIssueSearch.cpp` / `PlaneIssueSearch.cpp`) so a long sync can break a backoff on shutdown/supersede. All other sites pass `nullptr` — acceptable because every tracker HTTP call already runs on a **worker thread** (verified: `AnnotateAnalysisUi_Window.cpp:880` `// Pillar 2 ... → worker`; `TicketSyncService.cpp:605` runs `FetchIssuesStreamed` on the sync worker), so an uninterruptible ≤ ~7 s backoff never freezes the UI — worst case a background worker lingers on shutdown.

**Tests** assert the post-D3 target state directly (no need to characterize the soon-removed no-retry behaviour):

1. **Retry wrapper — pure unit** (`tests/Core/TrackerHttpRetry.test.cpp`, new): drive `TrackerHttpRequestWithRetry(requestFn, maxAttempts, cancelled)` with a lambda + captured counter; assert retry **count** + terminal **error kind** (not wall-clock — backoff is real `sleep_for`, not injectable). Unit-test `ClassifyTrackerResponse` over synthetic statuses (429→RateLimited, 5xx→ServerError, 401/403→Auth, 404→NotFound, other-4xx→InvalidRequest, ≤0→Transport). Cap `maxAttempts ≤ 3`; whole-file sleep budget < 1.5 s.
2. **Fixture extension** (`tests/support/JiraCatalogHttpFixture.h`): register `server_.Post/Put/Patch(".*")` → `Dispatch`; key scripting maps on `method + " " + path` (so GET-then-POST on the transitions endpoint script independently); new `method` param defaults to `"GET"` — existing GET callers compile + behave unchanged.
3. **Integration** (`tests/Core/JiraHttpFaults.test.cpp`, new): real `JiraClient` vs the fixture — (a) `startAt` page assembly (search-JQL + comment loops) reaches every page (`RequestCount` == page count); (b) GET 429-then-200 recovers (RequestCount == 2); GET forced-429 retries to `maxAttempts` then surfaces RateLimited; (c) **comment POST forced 5xx does NOT retry** (RequestCount == 1) — pins the safe-tier; (d) mutation happy-path verbs (transition POST, field PUT) reach the server.

## Files to modify

**Product (strict zone — `Source/Core/src/Tracker/` + matching include):**
1. `Source/Core/include/Tracker/TrackerHttpUtils.h:20-30` — add optional `cancelled`/`maxAttempts` params to the retryable helper signatures (default `nullptr` / `kTrackerHttpDefaultMaxAttempts`); keep the explicit-short-timeout GET overload no-retry.
2. `Source/Core/src/Tracker/TrackerHttpUtils.cpp:134-275` — wrap GET(normal)/PUT/PATCH in `TrackerHttpRequestWithRetry` (Transport+429+5xx); wrap POST Transport-only; probe GET overload stays maxAttempts=1; per-attempt `Record`+`Log` preserved. Include `TrackerHttpClient.h`.
3. `Source/Core/src/Tracker/JiraIssueSearch.cpp:30,341,411,450` + `PlaneIssueSearch.cpp:180,227,539,543,659` — thread `shouldCancel` into the fetch-loop `TrackerGetLogged` calls (interruptible backoff). Other sites unchanged.

**Test (`tests/`):**
4. `tests/support/JiraCatalogHttpFixture.h` — Post/Put/Patch routes + method-keyed scripting (default `"GET"`).
5. `tests/Core/TrackerHttpRetry.test.cpp` (new) — pure-unit retry + classify.
6. `tests/Core/JiraHttpFaults.test.cpp` (new) — loopback pagination + retry + no-retry-POST integration.
7. `tests/CMakeLists.txt` — register the two new TUs.

**Docs:**
8. `docs/guides/testing-surface.md` — §5 Gap 2 + §5.1 row 2 corrections (loopback reaches transport; retry now wired; void reframed to D2 only).
9. `docs/plans/testing-surface-roadmap.md` — fold recon #2 correction (retry was dead code, now wired), Slice A row ("already shipped via #1180"), Slice D row → D+D3.
10. `docs/self-improvement/categories/debt.md` — close the `debt.md:65` item; add no new D3 backlog (D3 done). Add a D2 follow-up entry (timeout/SSL/truncated-body seam).

**Grep-before-naming**: `rg -l 'TrackerHttpRetry|JiraHttpFaults' tests/` → neither TU exists. Retry helper lives only in `TrackerHttpClient.{h,cpp}`.

## Existing utilities reused

- `TrackerHttpRequestWithRetry` / `ClassifyTrackerResponse` / `TrackerHttpResult` — `Source/Core/include/Tracker/TrackerHttpClient.h:40,48` — the retry engine, now reused inside the helpers (no new retry logic).
- `TrackerError::IsRetryable` / `IsOk` / `Kind`, `TrackerErrorFromHttpStatus` — `TrackerError.h` — classification asserts. `IsRetryable()` already = Transport|RateLimited|ServerError; for the POST Transport-only path the helper checks `Error.Kind == TrackerErrorKind::Transport` explicitly rather than `IsRetryable()`.
- `NetworkUsageTracker::Record` + `LogTrackerHttpResult` — `TrackerHttpUtils.cpp:144,146` — kept per-attempt.
- `JiraCatalogHttpFixture` + `ScriptJson/ScriptHandler/ScriptStatus/RequestCount/Config` — `tests/support/JiraCatalogHttpFixture.h:36` — extended for mutations.
- doctest characterization conventions — `tests/Core/TrackerCatalogBuild.test.cpp:1` — TU template.
- `JiraClient::FetchIssuesStreamed` (`JiraClient.h:121`, threads `CancelCallback`) — the cancel-token source threaded into the search GETs.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no steady-state impact — retry adds latency only on *transient failure*, on worker threads, never on the success path or UI thread.
- **Pillar 2 (UI never blocks > 100 ms without cue)**: no impact — every tracker HTTP call already runs off-UI on a worker (verified annotations above); the ≤ ~7 s worst-case backoff cannot reach an ImGui draw. Probe overload stays no-retry so connectivity-state updates keep their 2 s/5 s budget.
- **Pillar 3 (never crash)**: positive — the empty-`requestFn` guard, `maxAttempts<1` clamp, and 429/500/transport degradation are now unit+integration covered.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (FIRES — diff touches `Source/Core/`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`:

1. **PR-fast CI** — scenario most directly exercising the path: the issue-sync / catalog-fetch scenario (sync drives `FetchIssuesStreamed` + `FetchFieldCatalog`). Confirm the exact name against `agents/core/perf-gatekeeper.md` § Curated diff → scenario map at push; run it via `scripts/dev/perf-run.sh` before opening the PR. Retry changes failure-path latency only — steady-state delta expected ≈ 0.
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`: the changed code is in `TrackerHttpUtils.cpp` (already worker-only); no new `ImGui::*`-reachable blocking path. No `PILLAR2_WORKER_ONLY` annotation needed (the seam was already worker-only).
3. **Dispatcher drain** — does not touch `MainThreadDispatcher::Drain()`.
4. **Visible-cue bucket-E harness** — no new > 100 ms *UI-thread* stall path (backoff is worker-side).
5. **Marker inventory** — adds no `SMATCHET_UI_PERF_SCOPE` markers.

**Pre-push local check**: `docs/guides/perf-workflow.md` § Gate-check vs baseline (Step 7) against the named sync scenario.
**Override**: `perf-out-of-band` only if a baseline-bump is intentionally queued (not expected here).

## Risks / non-goals

- **Duplicate mutation on retry** — *the* hazard. Mitigated structurally: POST retries on Transport-only (request never landed), never on post-send 429/5xx. Integration test (c) pins "comment POST 5xx ⇒ RequestCount==1".
- **Probe latency regression** — mitigated: the explicit-short-timeout GET overload keeps maxAttempts=1 (no retry). Risk: a non-probe site that happens to call the 5-arg timeout overload won't retry — acceptable (rare; correctness unaffected, just no retry there).
- **Backoff delays shutdown** on worker threads without a cancel token — mitigated for the long sync path (shouldCancel threaded); other sites accept a ≤ ~7 s worst-case worker lingering. Off-UI, so no freeze.
- **GitHub behaviour change** (retry-for-free) — accepted + documented; all GitHub GETs idempotent, POSTs Transport-only.
- **Real-sleep in retry-unit + integration tests** — capped `maxAttempts ≤ 3`, count/kind asserts not timing, file budget < 1.5 s (unit) / a few hundred ms (integration 429-then-200).
- **Fixture method-keying breaking GET callers** — mitigated: `method` defaults to `"GET"`; existing `TrackerCatalogBuild.test.cpp` suite must stay green.
- **Non-goal — D2** (timeout / SSL-handshake / truncated-body faults): need a real transport seam; httplib loopback can't cleanly synthesize them. Deferred, backlogged.
- **Non-goal — `IsTrackerTransportErrorText` string-table rework**: the new path classifies via `TrackerErrorFromHttpStatus` (status-based), not the string table; the table stays for legacy offline-detection callers.

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `TrackerHttpRetry.test.cpp` (count / kind / cancel / guards) + `JiraHttpFaults.test.cpp` (pagination, 429-then-200 recovery, forced-429 exhaustion, comment-POST-5xx no-retry, mutation verbs). Run via `cmake --build --preset ninja-test-msvc` → `ctest`.
- **Bucket E (ImGui Test Engine)**: N/A — no UI.
- **Sanitizer**: the new TUs + changed helper compile under `ninja-clang-asan` (exercises fixture thread/lifetime + retry-loop UB).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target — the helper change must compile in the DX12 world too; `TrackerHttpClient.h` is Core, no GLFW/GL include added).
- **Doc validation (blocks plan-doc PRs)**: `scripts/dev/test-docs.sh` green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs`**: run before finalizing; sharpen the "Transport-only POST" boundary + the probe-exclusion against the domain model. Outcome recorded pre-PR.
- **Manual residue**: none — all ctest-automated. Perf gate-check is scripted (`perf-run.sh`).

## Out of scope (flagged, not designed)

**Deferral residue-sweep**: grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray "retry already exercised" / "TrackerHttpRequestWithRetry dead/unwired" claims and correct them in this PR (roadmap recon #2 + `debt.md:65` are the known ones).

- **D2 — timeout / SSL-handshake-failure / truncated-body faults** — real `IHttpTransport` seam required. Deferred sub-slice; add a `debt.md` entry.
- **Plane mutation/pagination integration tests** — this slice integration-tests Jira; Plane's classification is unit-covered (`ClassifyTrackerResponse`) and its HTTP inherits the same helper retry, but a Plane loopback fixture is a separate follow-up.
- **`Retry-After` header honouring on 429** — the wrapper uses fixed exponential backoff; reading `Retry-After` is a future refinement, not in scope.

## Implementation log
- `377c9f72` · #1231 — HTTP fault-injection tests + wire retry into live paths (Slice D+D3): wired `TrackerHttpRequestWithRetry` into the live `TrackerHttpUtils.cpp` helpers, added `kTrackerHttpDefaultMaxAttempts` + `maxAttempts`/`cancelled` params, extended `JiraCatalogHttpFixture`, added `tests/Core/TrackerHttpRetry.test.cpp` + `tests/Core/TrackerHttpFaults.test.cpp`.

## Deviations from plan
- **Integration TU renamed**: the loopback integration TU shipped as `tests/Core/TrackerHttpFaults.test.cpp`, not the planned `tests/Core/JiraHttpFaults.test.cpp` (§ Approach test 3 + § Files-to-modify row 6).
- **Cancel-token threading deferred** (§ Files-to-modify row 3): threading the existing `shouldCancel` token into the Jira/Plane search-fetch GET loops (`JiraIssueSearch.cpp` / `PlaneIssueSearch.cpp`) was NOT landed. The worker-thread no-token fallback is acceptable (every tracker HTTP call already runs off-UI, so the ≤ ~7 s backoff never freezes the UI; worst case a background worker lingers on shutdown); deferred as hardening.
- **Doc-update rows 8-10 deferred** (§ Files-to-modify): the `docs/guides/testing-surface.md`, `docs/plans/testing-surface-roadmap.md`, and `docs/self-improvement/categories/debt.md` updates were NOT landed in #1231 — deferred follow-up tracked in `debt.md`.

## Verification (actual)
- **Bucket A (pure-logic ctest)**: `tests/Core/TrackerHttpRetry.test.cpp` + `tests/Core/TrackerHttpFaults.test.cpp` verified present in tree (archival audit 2026-06-16), not re-run.
- **Retry wired live**: `TrackerHttpRequestWithRetry` wired into the live `TrackerHttpUtils.cpp` helpers with `kTrackerHttpDefaultMaxAttempts` + `maxAttempts`/`cancelled` params; `JiraCatalogHttpFixture` extended for mutations — verified present in tree (archival audit 2026-06-16), not re-run.
- **Build / sanitizer / perf / doc gates**: not independently re-run during the archival audit; merged under #1231's CI.
