- 2026-09-29 · offline-first calibration (S13) · [debt] · P3 — the bulk-import hydration prefetch retries a failed fetch every frame

  Details: `Ui/SmatchetBulkTicketsUi.cpp` calls `app.PrefetchIssueTicketsForKeys(keysNeedingHydration)`
  on every frame the Bulk Import panel draws. `AppController::PrefetchIssueTicketsFrom` only
  de-duplicates keys that are still in flight. When a fetch fails, its keys are released and the next
  frame launches the same fetch again, with no backoff. While the tracker is reachable but failing
  (5xx, rate limit), that is a continuous stream of doomed requests for as long as the panel is open.
  Since S13 the prefetch is skipped while the tracker is known offline, so the offline case is covered.
  Found by the offline-first calibration review (`offline-network-read-ungated` hit on
  `AppController_TicketPrefetch.cpp`).

  Concrete next action: route the prefetch through a `KeyedLookupCache` keyed by
  (backend key, issue key), or keep a per-key retry-after next to `bulkImportPrefetchKeysInFlight_`.
  After a failure the key then waits out `kLookupRetryAfterSeconds`, and `OnConnectivityRecovered`
  clears the wait.
  Status: open
  Last-reviewed: 2026-09-29
