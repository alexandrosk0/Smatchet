# Plan — Coverage-gap Tier 2: backend-shell fixture tests (Plane + Linear issue search)

> **Slug**: `coverage-gap-tier2-backend-shell-fixtures`
>
> **Status**: `active`

## Context

[`TEST_COVERAGE_GAP_MAP.md`](../../../TEST_COVERAGE_GAP_MAP.md) Tier 2 covers the backend client HTTP
shells (~3.4K LOC): the mapping/JQL-translation halves of every backend are well tested, but
"pagination, retry/error classification, and request orchestration" are not. The map's prescription is
one fixture test per shell, with `JiraIssueSearch.cpp` / `TrackerCatalogBuild.test.cpp` as the proven
template — a real client driven at an in-process `httplib::Server` loopback over real cpr HTTP
(`tests/support/JiraCatalogHttpFixture.h`). The two highest-priority shells are flagged in the map
itself: `PlaneIssueSearch.cpp` (708 LOC, **audit-flagged**) and `LinearIssueSearch.cpp` (374 LOC,
**security-flagged**). Neither TU is compiled into any test target today, so their pagination caps
(the bounded-loop DoS guards), response classification (non-200 / HTML / invalid-JSON / no-results),
and streaming/cancellation protocols are regression-invisible.

The Tier-1 campaign closed in #1617; this plan starts the Tier-2 lane the map's "Recommended
sequencing" item 3 calls for, sequenced ahead of the B2 `TrackerHttpClient` migration batches that
will churn exactly this orchestration code.

## Approach

No production-code changes — this is pure test-coverage work over the existing shells, using the
loopback-fixture pattern already shipped for Jira. The generic `JiraCatalogHttpFixture` gains one
additive hook (`ScriptRaw`: exact status + raw body + content type) so tests can serve the HTML /
invalid-JSON / empty-body / non-200-with-detail responses the Plane classifier branches on; existing
scripted-JSON routes are untouched. `PlaneClient` is instantiated for real (all five Plane
implementation TUs join `SmatchetTests`, mirroring the six-TU Jira block), driven via
`FetchIssuesStreamed`'s `configOverride` seam pointed at the loopback. Linear's shell is free
functions (`smatchet::linear::FetchIssuesViaGraphQl` / `FetchIssuesForKeysViaGraphQl`) taking the API
URL directly, so only `LinearIssueSearch.cpp` itself needs linking — its helper deps are already in
the rig.

## Files to modify

### Slice 1 (this PR) — Plane + Linear issue-search shells

1. `tests/support/JiraCatalogHttpFixture.h` — additive `ScriptRaw(path, status, body, contentType, method)`
   route kind (checked before fixed/dynamic routes); everything else unchanged.
2. `tests/Core/PlaneIssueSearchHttp.test.cpp` — NEW suite driving a real `PlaneClient` at the loopback:
   two-page cursor pagination (batch stream + `FullSyncCompleted` + per-endpoint request counts),
   self-referential-cursor page cap (50) soft-warning termination, project-resolve failure surface,
   non-200 with `{"detail":...}` extraction + 404 hint, HTML-body / invalid-JSON / empty-body / missing
   `results` classification, structured-query key filter, `FetchIssuesForKeys` early-exit cancellation
   (page-2 never fetched), and the `ProbeReachability` classification matrix (200 / 401 / 500 / 404 /
   transport-down — pinning the §2.1 P1 "stale base URL misclassified as TransportDown" fix).
3. `tests/Core/LinearIssueSearchHttp.test.cpp` — NEW suite driving the GraphQL shell at the loopback:
   two-page cursor pagination with the `onPage`/`isLast` streaming protocol, GraphQL `errors[]`-on-200
   fatal classification (terminal page still emitted), invalid JSON, missing `data.issues`, HTTP 400
   fallback message, `endCursor`-missing-while-`hasNextPage` early stop + warning, page cap (20)
   warning, request-body wire shape (team scope + translated filter + title term; `after` cursor
   threading), `FetchIssuesForKeysViaGraphQl` invalid-key `Err` with zero HTTP calls, or-disjunction
   wire shape, and the 250 `first` clamp.
4. `tests/CMakeLists.txt` — link `PlaneClient.cpp` + `PlaneIssueSearch.cpp` + `PlaneIssueMutation.cpp` +
   `PlaneFieldCatalog.cpp` + `PlaneActivityFeed.cpp` (full vtable, Jira-block precedent) and
   `LinearIssueSearch.cpp` into `SmatchetTests`; register both new test TUs. `SmatchetTsanTests` is
   untouched (both shells pull `<cpr/cpr.h>`, which the TSan subset excludes by design).

### Slice 2 (second PR) — GitHub issue-search + client shells

5. `tests/Core/GitHubIssueSearchHttp.test.cpp` — NEW suite driving the `smatchet::github` free
   functions at the loopback: two-page GraphQL pagination + `after`-cursor wire threading + the
   `repo:` scope in `variables.q`, fatal page classification (errors[]-on-200 / non-200 message
   extraction / invalid JSON / missing `data.search`) with the single-terminal-emit contract, the
   `endCursor`-missing early stop, the 10-page cap, the `key =` post-filter (pages and aggregate),
   two-source orchestration (commits-only routes to REST `/commits` with zero GraphQL calls;
   commit-fetch failure is a soft warning that still blocks the full-sync claim), and the per-key
   single-issue GET error taxonomy (Auth / InvalidRequest / NotFound / Parse / success).
6. `tests/Core/GitHubClientHttp.test.cpp` — NEW suite driving a real `GitHubClient` (ctor'd with a
   deliberately bogus base URL so every hit proves the issue-#979 live-cfg resolution): the
   `/rate_limit` probe classification matrix (missing-PAT fail-fast, 200 + core-quota diagnostic,
   401/403 hints, 404 base-URL hint), comment-post wire shape + error taxonomy (2xx body, malformed
   key, cleared-live-PAT-no-ctor-fallback, 422 message extraction), the streamed-fetch shim
   (batches + summary wiring), and the per-key shim. The cfg-less paths (FetchIssueComments /
   CreateIssue / UpdateField) resolve from on-disk ConfigManager and stay out of scope.
7. `tests/support/HttpRequestCapture.h` — NEW shared thread-safe request-body capture (extracted
   from the Slice-1 Linear suite's file-local struct so Slice 2 doesn't clone it; the Linear suite
   now consumes it too).
8. `tests/CMakeLists.txt` — register both suites; link `GitHubIssueSearch.cpp` + `GitHubClient.cpp` +
   `GitHubActivityFeed.cpp` (the GitHubClient vtable spans the latter two; all other deps were
   already in the rig).

### Slice 3 (third PR) — Plane + Linear mutation (write) shells

9. `tests/Core/PlaneIssueMutationHttp.test.cpp` — NEW suite driving the real `PlaneClient` write
   surface at the loopback: PATCH wire shape (payload verbatim to the work-item route), the
   visual-key→UUID cache miss (`InvalidRequest` + refresh hint, zero HTTP), non-2xx error-body
   extraction + status-kind mapping, create visual-key derivation (`SMT-<seq>`) **and** the
   create→cache-warm→immediate-visual-key-edit seam, the created-key-unknown `Ok(empty)` contract,
   comment-post markdown→`comment_html` conversion, comments-fetch envelope unwrap + status mapping.
   The cfg-less paths run under `TestEnvGuard` with the loopback config saved into the guard's dir.
10. `tests/Core/LinearIssueMutationHttp.test.cpp` — NEW suite for the GraphQL write shell: the
    identifier→UUID resolve hop fronting every write (wire shapes of both requests asserted from
    captured bodies), resolve-miss `InvalidRequest` naming the identifier (mutation never fires),
    `success=false` + `errors[]` taxonomies, create payload guard / identifier extraction /
    created-key-unknown, comment resolve+`commentCreate` wire shape (markdown verbatim), and the
    issue-#979 cleared-live-key-no-ctor-fallback rule. Same `TestEnvGuard` pattern for cfg-less paths.
11. `tests/CMakeLists.txt` — register both suites; link `LinearClient.cpp` + `LinearIssueMutation.cpp`
    (the LinearClient vtable; every other dep, including the already-linked `PlaneIssueMutation.cpp`,
    was in the rig).

### Later slices (not this PR)

- `GitHubCommits.cpp` (Vcs, 96 LOC) and the remaining small shells if a future batch wants them;
  otherwise the Tier-2 lane is complete and the map's item 4 (Commands harness) is next.

## Verification

- Local (this container): `SmatchetTests` now configures and builds on Linux via the
  `posix-core-check` preset with `-DSMATCHET_BUILD_TESTS=ON -DSMATCHET_WITH_AI=ON` and
  `-DFETCHCONTENT_SOURCE_DIR_CURL=<git clone of curl-7_80_0>` (the release-tarball URL is blocked by
  the session proxy; the git tag is identical content for the CMake build). Run the two new suites via
  doctest test-suite filters.
- CI (the real gate): the Windows `SmatchetTests` lanes compile + run both suites; repo lint gates
  (`test-lint-rules.sh --diff`, docs gate, clang-format) run locally before push.

## Implementation log

- `e551c492` · Slice 1: Plane + Linear issue-search fixture suites + `ScriptRaw` fixture hook +
  rig registration (13 cases / 107 assertions; verified green on the Linux `SmatchetTests` build
  described in § Verification, plus lint/docs/format gates). Merged in #1622 (`86ae8393`).
- `fe7a3a58` · Slice 2: GitHub issue-search + client fixture suites + shared `HttpRequestCapture`
  support header (13 cases / 117 assertions; same Linux verification + gates; full rig 2202/2203
  with the sole failure the pre-existing `SubprocessCapturePure` wide-char case). Merged in #1628
  (`5b92ad45`).
- `9e039ce9` · Slice 3: Plane + Linear mutation-shell fixture suites (6 cases / 62 assertions;
  same Linux verification + gates; full rig 2216/2217, same pre-existing sole failure). First
  combined use of `TestEnvGuard` + the HTTP loopback fixture for the cfg-less write paths.

## Deviations from plan

- Plan-lock seed push to `refs/locks/*` is blocked by the session git proxy (403), same as Slices 1–3
  of the Tier-1 plan; logged here per the seed contract and continued.
- None functional — the slice landed as planned.
