# Plan — Retire the IsTrackerTransportErrorText string heuristic (N12)

> **Slug**: `retire-transport-error-text`
>
> **Status**: `active`

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

### Slice 2 (next)

12. `ITrackerIssueReader.h` fetch summary + the four backends' streamed fetch error composition → structured kind; `AppController.cpp` pack composer consumes it.
13. Catalog-error path (`FocusedFieldCatalogError`) + `OfflineQueueService` replay classification (`.cpp:980/1226`) + field-edit `ApplyResult` + the two UI consumers + `AppController.cpp:547` / `AppController_CatalogAndFieldEdit.cpp:339` + `LinearClient.cpp:184`.

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

- Slice 1 (this PR): landed per § Files to modify items 1–11. Classification seams: pack composer (`AppController.cpp`), streaming worker summary copy + exception paths (`TicketSyncService.cpp`), probe-loss write (`ConnectivityMonitorService.cpp`, `true` by construction). Consumers de-classified: `ApplyIssueFetchPack`, `TickStreamingApply`, `IsConnectivityDegradedForProbeInterval` (warning half).

## Deviations from plan

- None yet.

## Verification (actual)

- (populate per slice; slice 1: `ninja-tsan-linux` ctest run recorded in the PR body test plan.)

## Archive (post-ship — DO IN THIS PR, never a follow-up)
Archive when slice 3 ships: flip § Status to `shipped`, populate the sections above, `git mv` to `docs/plans/shipped/`.
