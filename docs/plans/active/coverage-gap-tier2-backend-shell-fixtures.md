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

### Later slices (not this PR)

- `GitHubIssueSearch.cpp` / `GitHubClient.cpp` fixture suite (same pattern; largest remaining shell).
- Mutation-side suites (`PlaneIssueMutation.cpp`, `LinearIssueMutation.cpp`) once the B2 batches reach
  the write paths.

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
  described in § Verification, plus lint/docs/format gates).

## Deviations from plan

- Plan-lock seed push to `refs/locks/*` is blocked by the session git proxy (403), same as Slices 1–3
  of the Tier-1 plan; logged here per the seed contract and continued.
- None functional — the slice landed as planned.
