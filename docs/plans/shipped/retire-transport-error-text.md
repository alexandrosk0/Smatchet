# Plan — Retire the IsTrackerTransportErrorText string heuristic (N12)
<!-- plan-date: 2026-07-11 -->

> **Slug**: `retire-transport-error-text`
>
> **Status**: `shipped`

## Context

[`backlog/BACKLOG_CODE_REVIEW.md`](../../../backlog/BACKLOG_CODE_REVIEW.md) §N12, unblocked by B2's completion (2026-07-05): `IsTrackerTransportErrorText` (`Source/Core/src/Tracker/TrackerHttpPure.cpp`) classifies error **text** by substring matching, shadowing the structured `TrackerError`/`ClassifyTrackerResponse` mechanism, across ~10 consumers. Worst shape: `ConnectivityMonitorService::IsConnectivityDegradedForProbeInterval` re-classified `lastTicketSyncWarning_` — a message that upstream code had already classified, PREFIX-composed ("Showing cached issues — …"), and handed over — which only worked because the heuristic happens to be substring-based. Consumers re-deciding a question the producer already answered is the N12 defect class.

## Approach

Three slices, ordered by blast radius. **Slice 1 (this PR)** — *classification travels with the error*: transport-ness is decided ONCE at the seam that composes each error (`FetchIssuesForActiveView` pack composer, the streaming worker's summary→state copy, the connectivity probe's by-construction write) and carried as a `bool …Transient` flag; every downstream consumer branches on the flag and never re-classifies text. The heuristic still runs — but at exactly the owned producer seams, with byte-identical classification behaviour. **Slice 2** — *structured classification at the source*: the backends' fetch summaries and the catalog-error path carry `TrackerError` kinds instead of calling the heuristic on flattened text; same for the mutation/replay consumers (`OfflineQueueService`, field-edit pipeline `ApplyResult`, `SmatchetBulkTicketsUi`, `SmatchetNewIssueDraftUi`, `AppController` §547/§339, `LinearClient::ProbeReachability` which has the status code in scope already). **Slice 3** — delete `IsTrackerTransportErrorText` + its tests once `git grep` shows zero production callers.

## Files to modify

### Slice 1 (this PR)

1. `Source/Core/include/Sync/SyncTypes.h` — `TrackerIssueFetchPack` += `bool FetchErrorTransient`.
2. `Source/Core/src/AppController.cpp` `FetchIssuesForActiveView` — classify once at pack composition.
3. `Source/Core/include/Sync/TicketSyncService.h` — `StreamingSyncState` += the flag (written with `FetchError` under `QueueMutex`).
4. `Source/Core/src/Sync/TicketSyncService.cpp` — worker-side classification seam (summary copy + both exception paths), the three per-sync resets, `ApplyIssueFetchPack` + `TickStreamingApply` branch on the flag, warning setter calls carry it.
5. `Source/Core/include/ITicketSyncDeps.h` — `SetLastTrackerTicketSyncWarning(message, transient)`.
6. `Source/Core/include/GridContextDepsAdapter.h` + `src/GridContextDepsAdapter.cpp` — forward the flag.
7. `Source/Core/include/ConnectivityMonitorService.h` + `src/ConnectivityMonitorService.cpp` — `lastTicketSyncWarningTransient_` member set/cleared with the message everywhere it's written (probe-loss write is `true` by construction); `IsConnectivityDegradedForProbeInterval` reads the flag for the warning half (the catalog-error half keeps the heuristic until slice 2's catalog work).
8. `tests/support/FakeTicketSyncDeps.h` — new setter signature + captured flag.
9. `tests/Core/TicketSyncService.test.cpp` — flag-authority regression case (transport-shaped TEXT with flag false must NOT flip connectivity; flag true with arbitrary text must).
10. `tests/Core/ConnectivityMonitorService.test.cpp` — setter call sites carry the flag.
11. `backlog/BACKLOG_CODE_REVIEW.md` — N12 row/section note slice 1 + link here.

### Slice 2 (next) — PRECONDITION discovered 2026-07-11: kind-reliability audit first

**`TrackerError.Kind` is not yet authoritative at the producers.** Evidence: `FakeTrackerClient::FetchIssuesForKeys` returns `TrackerErrorInvalidRequest(<any text>)` — the replay tests' "timeout is transient" case passes ONLY because the code sniffs the TEXT; with `.IsRetryable()` it would classify non-transient. Production has the same class: `PlaneClient::UpdateIssueFields` wraps `ResolvePlaneProject` failures as `TrackerErrorUnknown` with an explicit `TODO(#21b later slice): re-thread status when a consumer reads .Kind`. Swapping consumers to `.Kind`/`.IsRetryable()` before auditing every `TrackerError` construction site across the four backends would silently flip replay/banner classification wherever a kind is wrong.

11a. **Kind-reliability audit** — sweep every `TrackerError{…}` / `TrackerError…(...)` construction in `Source/Core/src/Tracker/` (+ the test fakes) and make each carry the correct kind (finish #21b's status re-threading); pin with per-backend fixture cases (transport / 429 / 5xx / 404 / 4xx). Only then:

> **Fetch-path audit executed 2026-07-11:** `JiraClient::FetchIssuesForKeys`, the GitHub REST and Linear GraphQL fetch-by-key paths already classify correctly (`TrackerErrorFromHttpStatus`: status ≤ 0 → Transport; `TrackerErrorParse` on body failures) — no work there. The two real holes: (1) **streamed-fetch string flattening** — `PlaneClient::FetchIssuesForKeys` wraps `summary.FetchError` as `TrackerErrorUnknown` because `TrackerIssueFetchSummary` / `FetchIssues(outFetchError*)` carry only text; fixing this IS item 12 and means the `ITrackerIssueReader::FetchIssues` out-param goes structured (4 backends + 3 fixtures + callers). Until then a consumer swap would REGRESS Plane: a transport outage during replay re-fetch would classify non-retryable (Unknown) and record a conflict instead of retrying. (2) **`FakeTrackerClient::SetFetchIssuesForKeysResult`** hardcodes `TrackerErrorInvalidRequest` — the fake needs kind scripting (accept a `TrackerError`) so the replay suites can pin structured classification. Item 12 is therefore the critical path; the consumer swaps (item 13) are mechanical after it.

12. `ITrackerIssueReader.h` fetch summary + the four backends' streamed fetch error composition → structured kind; `AppController.cpp` pack composer consumes it. **Error-origin map (audited 2026-07-11, all have the HTTP status in scope at the composition site):** Jira — `LogAndBuildPageFetchError(page, response)` + the `JqlSearchOutcome.FetchError` per-page path (+ a missing-config InvalidRequest); Plane — `summary.FetchError = page.Error` inside the streamed page loop; GitHub/Linear — the inner `fetchError` strings their streamed wrappers flatten from the REST/GraphQL loops (Linear also has the fixed `kApiKeyMissingError`). Shape: add `TrackerError Error` to `TrackerIssueFetchSummary` + an optional `TrackerError*` out-param on `FetchIssues` (string param kept transitionally so the many test call sites stay untouched); each backend fills both at those sites; `PlaneClient::FetchIssuesForKeys` returns `summary.Error` instead of wrapping Unknown; the slice-1 seams prefer `summary.Error` when `Kind != None` and keep the text heuristic as fallback.
13. Catalog-error path (`FocusedFieldCatalogError`) + `OfflineQueueService` replay classification (`.cpp:980/1226` — `FetchIssuesForKeys`' `TrackerError` and `UpdateIssueFields`' `updateErr` are already in scope at both callers, so this is mechanical once 11a lands) + field-edit `ApplyResult` + the two UI consumers + `AppController.cpp` prefetch (`FetchAndCachePrefetchedTickets` — `fetchResult.error()` in scope) / `AppController_CatalogAndFieldEdit.cpp` (`SetFieldCatalog` needs the kind plumbed through its string seam) + `LinearClient.cpp` probe (careful: its text sniff currently also matches GraphQL error text on a 200 — parity needs a deliberate decision).

### Slice 3 (last)

14. Delete `IsTrackerTransportErrorText` from `TrackerHttpPure.{h,cpp}` + its doctest block + the `TrackerHttpPure` HIGH_RISK_UNITS note if the unit's line count shifts its rate.

## Existing utilities reused

- `IsTrackerTransportErrorText` itself (transitionally, at producer seams only) — byte-identical classification, zero behaviour change in slice 1.
- `FakeTicketSyncDeps` / `FakeConnectivityDeps` fixtures — no new test infra.

## Extraction sizing

Slice 1: +1 bool on two structs and one service member, one virtual signature change with two implementers; no function approaches a cap.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — a bool copy alongside existing string copies.
- **Pillar 2 (UI never freezes)**: no impact — no new blocking work; same UI-tick single-writer discipline (flag written wherever the string already was).
- **Pillar 3 (never crash)**: risk reduced — the fragile classify-a-composed-string dependency (works only while the heuristic is substring-based) is gone at the connectivity seam.
- **Pillar 4 (accessibility)**: no UI change.

## Perf-review-system gates

1. **PR-fast CI**: N/A — no hot-path shape change.
2. **Pillar 2 static scanner**: no new sync-I/O reachable from `ImGui::*`.
3. **Dispatcher drain**: untouched.
4. **Visible-cue bucket-E harness**: no new stall path.
5. **Marker inventory**: no new markers.

**Pre-push local check**: N/A — no perf-relevant change (see gate 1).

## Risks / non-goals

- **Risk**: a pack composer forgets to classify → flag defaults `false` → a real outage shows no banner/TransportDown flip. Mitigation: exactly one composer exists (`FetchIssuesForActiveView`) plus the streaming seam, both classified in this slice; the flag-authority doctest pins the contract.
- **Non-goal (slice 1)**: changing WHAT classifies as transport — classification behaviour is byte-identical (same heuristic, same inputs, moved to producer seams).
- **Non-goal (slice 1)**: the mutation/replay/UI consumers and the catalog-error half — slice 2 (they flatten `TrackerError` upstream; fixing them properly is structured-kind plumbing, not flag-forwarding).

## Verification

- **Bucket A (ctest, Linux TSan subset)**: `TicketSyncService.test.cpp` (incl. the new flag-authority case) + `ConnectivityMonitorService.test.cpp` green under `ninja-tsan-linux` locally; full rig in CI.
- **Build gate**: CI dual-target + POSIX lanes.
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Manual residue**: none — behaviour-preserving refactor pinned by tests.

## Out of scope (flagged, not designed)

- Slices 2–3 (§ Files to modify items 12–14) — tracked here; N12 stays OPEN in the backlog until slice 3 deletes the heuristic.

## Implementation log

- Slice 3 (fifth PR, final): the heuristic is deleted. Closed Plane's last unclassified paths: `ResolvePlaneProject` gained an optional `TrackerError* outClassified` filled at its four failure sites (HTTP status via the guarded `TrackerErrorFromHttpStatus`, invalid-JSON / no-results-array → Parse, lookup-miss → NotFound(status 0)); all six call sites (streamed search, mutation ×3, field catalog, activity feed) pass the kind through instead of wrapping Unknown; `FetchIssuesStreamed`'s outer catches classify Parse / Unknown (deliberately non-retryable, 13a precedent — transport surfaces as classified statuses, never throws). The 13a/13b consumer swap also exposed the Jira mutation gap the old `TODO(#21b)` markers deferred: `UpdateIssueFieldsViaTransition` / `UpdateIssueFieldsViaPut` / `AddIssueToSprint` now classify at their own failure sites (guarded HTTP status / Parse / InvalidRequest / Auth) instead of collapsing to Unknown — without this, a Jira transport failure during a field edit would have stopped reaching the offline queue (`IsRetryable()` false); pinned by the new `tests/Core/JiraIssueMutationHttp.test.cpp` (400 → InvalidRequest non-retryable, 503 → ServerError retryable, local validation → InvalidRequest). The two seam fallbacks then dropped the sniff (`AppController.cpp` pack composer, `TicketSyncService.cpp` worker copy — `summary.Error.IsRetryable()` only; the worker exception path is deliberately non-transient, 13a precedent). `IsTrackerTransportErrorText` deleted from `TrackerHttpPure.{h,cpp}` + its doctest block; orphaned includes removed (TicketSyncService, OfflineQueueService, LinearClient, replay test); every reference remaining in the tree is a tombstone comment.

- Slice 2 / item 13b (fourth PR): the string-seam consumers swapped to structured classification. Field-edit chain: `FieldEditResult` += `ErrorTransient` (filled from the mutation `TrackerError`'s `IsRetryable()` at all four `FieldEditPipelineService` fill sites; the editmeta-refresh local message is non-transient), consumed by `SmatchetGridFieldEditPipeline`'s offline-queue fallback; pinned by a `[high-risk]` 3-subcase test scripting kinds through the fake's new `EnqueueUpdateIssueFieldsError(TrackerError)`. Create chain: `IssueCreateResult` += `ErrorTransient` (both network sites in `IssueCreatePipeline`; the created-key-unknown outcome is deliberately NON-transient — the create succeeded server-side, queueing a retry would duplicate the issue), consumed by `SmatchetBulkTicketsUi` + `SmatchetNewIssueDraftUi`. Catalog chain: `errorTransient` travels `RefreshFieldCatalog` flatten seam (`catalogResult.error().IsRetryable()`) → `SetFieldCatalog`/`HandleFieldCatalogError` → `GridLiveContext.LastTrackerFieldCatalogErrorTransient` (the composed no-cache store is transport-by-construction) → `IConnectivityDeps::FocusedFieldCatalogErrorTransient()` → `ConnectivityMonitorService`'s catalog half. `LinearClient::ProbeReachability` now checks `!resp.error.message.empty()` instead of sniffing the body text — a deliberate semantic fix (a GraphQL error body mentioning "timeout" on HTTP 200 no longer misreads as an outage). Remaining for slice 3: the two seam fallbacks (`AppController.cpp` pack composer, `TicketSyncService.cpp` worker copy + exception path) blocked on Plane's unclassified resolve/exception paths, then delete the heuristic.

- Slice 2 / item 13a (third PR): the consumers holding a `TrackerError` swapped to structured classification — `AppController_TicketPrefetch` (log-level routing via `IsRetryable()`), `OfflineQueueService` conflict re-fetch (retry-vs-ask decided by the `FetchIssuesForKeys` Result's kind; a THROWN re-fetch now classifies non-transient — a deliberate change: exceptions are bug shapes, and the non-transient branch is the safe ask-the-user path) and `HandleFieldEditUpdateFailure` (signature takes the mutation's `TrackerError`; transport-cap vs replay-rejected decided by `IsRetryable()`). The replay suite's transient case now scripts `TrackerErrorTransport` via the fake's kind scripting. Remaining for 13b: the string-seam consumers (field-edit `ApplyResult`, the two UIs, the catalog-error path + `ConnectivityMonitorService`'s catalog half, `LinearClient::ProbeReachability`).

- Slice 2 / item 12 (second PR): the structured-kind plumbing landed per the audited error-origin map. `TrackerIssueFetchSummary` += `TrackerError Error`; `FetchIssues` gained the optional `outFetchErrorStructured` out-param (string param kept — test call sites untouched); all four backends fill both at their audited composition sites (Jira: page-HTTP + parse-outcome + missing-config; Plane: the page-fetch classifier via a `Classified` field on `PlaneIssuePageFetch`, config-validation, with resolve/exception paths left unclassified → heuristic fallback; GitHub: PAT-missing/setup-fatal/GraphQL page loop; Linear: API-key-missing (Auth, matching the ForKeys path) + the parse-fatal branch which has the status in scope). `PlaneClient::FetchIssuesForKeys` now returns `summary.Error` when classified (the audited Plane replay regression is closed). The two slice-1 seams (pack composer, streamed worker copy) prefer the structured kind and keep the text heuristic only as the unclassified-path fallback. Fixtures classify their `loadError_` as InvalidRequest (matching their ForKeys paths); `FakeTrackerClient` gained kind scripting (`SetFetchIssuesError` / `SetFetchIssuesForKeysError` + `ScriptedFetchResult.FetchErrorStructured`). Pinned by a new `[high-risk]` case: a fetch failure whose TEXT the heuristic would not call transport but whose structured kind is Transport must flip connectivity to TransportDown.
- Slice 1 (this PR): landed per § Files to modify items 1–11. Classification seams: pack composer (`AppController.cpp`), streaming worker summary copy + exception paths (`TicketSyncService.cpp`), probe-loss write (`ConnectivityMonitorService.cpp`, `true` by construction). Consumers de-classified: `ApplyIssueFetchPack`, `TickStreamingApply`, `IsConnectivityDegradedForProbeInterval` (warning half).

## Deviations from plan

- Slice 2 split into three PRs (item 12, 13a, 13b) instead of one — the kind-reliability precondition (§ Slice 2 header) made item 12 the critical path and the consumer swaps land safely only after it.
- Slice 3 grew the Jira mutation re-threading (`Via*` helpers, `AddIssueToSprint`) that item 11a's audit had scoped to the fetch path only — the 13b consumer swap made the mutation kinds load-bearing (field-edit offline-queue fallback), so deferring them further would have shipped a Jira replay regression.
- Three deliberate behaviour changes, each documented in-code: thrown paths classify non-transient (13a precedent; previously the text sniff could read a timeout-shaped `what()` as transport); `LinearClient::ProbeReachability` keys on `resp.error.message` instead of body text (a GraphQL error body mentioning "timeout" on HTTP 200 no longer misreads as an outage); the created-key-unknown create outcome is non-transient (queueing a retry would duplicate the issue).

## Verification (actual)

- Slices 1/12/13a/13b: `ninja-tsan-linux` suite green per PR (#1738 / #1755 / #1758 / #1762), incl. the flag-authority, kind-vs-text witness, and ErrorTransient classification cases.
- Slice 3: `ninja-tsan-linux` 226 cases / 2148 assertions green; TSan case list byte-identical to develop tip (the deleted doctest block was not in the TSan subset). `git grep IsTrackerTransportErrorText` → tombstone comments only. New `JiraIssueMutationHttp.test.cpp` kind cases run in the full CI rig (cpr chain unbuildable locally).
- Doc validation: `scripts/dev/test-docs.sh` green per PR.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
Archived with slice 3 (status flipped to `shipped`, moved to `docs/plans/shipped/`).
