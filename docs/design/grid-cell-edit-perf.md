# grid-cell-edit-perf — async cell-edit commit (pillar 1 + 2 restoration)

## Problem

User report (verbatim): *"When I edit any value, there is a pause, all updating need to happen on the background and the user should be able to edit fields unrestricted in 144Hz without any pauses."*

The grid-cell commit path in `ProcessGridFieldEdits` runs `app.SubmitFieldEditNetworkOnly(...)` **synchronously on the UI thread**. That call funnels into `Backend->UpdateIssueFields(...)` — a full HTTP roundtrip via `cpr` against Jira / Plane. A typical Jira `PUT /issue/{key}` takes 80-400 ms even on a fast connection; under packet loss or backend latency it climbs into multi-second territory. While the call is in flight the UI thread is blocked: ImGui frame production stalls and the user sees the entire app freeze for one or more cell-edit commits, with no spinner, no progress, no responsiveness.

This violates:

- **Pillar 1 (perf, 144 Hz, frame budget 6.94 ms)** — a single edit can cost 100-400× the per-frame budget on the UI thread.
- **Pillar 2 (UI never freezes — sync I/O reaching ImGui frame = CRITICAL)** — `cpr::Put` on the UI thread, reachable from `SmatchetActiveProjectGridUi::Render` → `ProcessGridFieldEdits`.

## Root cause

`Source_Core/src/SmatchetGridFieldEditPipeline.cpp:79`:

```
if (app.SubmitFieldEditNetworkOnly(edit.IssueId, edit.Field, edit.Values, ...))
```

Called from `ProcessGridFieldEdits`, which is called per-frame from `SmatchetActiveProjectGridUi::Render` on the UI thread. The subsequent `ApplyFieldEditResult` (cache write — cheap), `QueueFieldEditOffline` (SQLite write — also synchronous but tens of ms at worst), and toast push are all on the same UI-thread tick.

The dominant cost is the HTTP call; everything else is a rounding error by comparison, but they pile up too.

## Fix

Move the entire commit pipeline (HTTP call + offline-queue fallback + result apply + toast) to a background worker via the existing `AppController::LaunchBackgroundTask` + `mainThreadDispatcher.PostToMainThread` pattern that `AiAssistantController`, `TicketSyncService`, and `BackendAuditTrail` already use.

Concretely, `ProcessGridFieldEdits` becomes a tiny state-machine that:

1. On a fresh queued edit + no worker in flight: snapshot the original-estimate / remaining-estimate / issue-type fields from the cached ticket (cheap, UI-thread copy of pre-existing `std::string` members), set the **Saving** feedback chip, then `LaunchBackgroundTask` with a copy of the edit + snapshots.
2. The worker calls `SubmitFieldEditNetworkOnly` (HTTP — fine, not on UI thread), and on transport failure falls back to `TryPrepareOfflineFieldEdit` for queue eligibility.
3. The worker posts a single completion lambda via `mainThreadDispatcher.PostToMainThread`. On the UI thread the post-back runs the offline-queue write (still cheap SQLite — tens of µs), `ApplyFieldEditResult` (cache mutation), toast push, feedback chip update, and clears `hasInFlightEdit` so the next queued edit dispatches.
4. **Optimistic local-display update**: the moment the worker is dispatched, write the expected new values straight into the cached ticket so the cell reads back what the user typed without waiting for HTTP. The eventual server result either confirms (no-op) or corrects the display (rare — server-side rejection). For simplicity in slice 1 we keep the existing semantics: cell display reflects the server's response, but the **Saving** chip is shown immediately and the UI never freezes. Optimistic display is a follow-up if real-world feedback shows users want the cell to read-back instantly.

Cancellation atom not required for this path — the worker writes through `AppController` which already protects against late-arriving callbacks via `shuttingDown_`. A per-edit cancel atom is overkill given the worker holds copies of all data and never directly touches UI state outside the `PostToMainThread` block.

## CLI surface — `debug.grid.edit-burst`

A new debug command exercises the path headlessly:

```
debug.grid.edit-burst --field <id> --row <key|index> --new-value <str> --count <N> [--use-cache-row-zero]
```

Returns JSON:

```json
{
  "command": "debug.grid.edit-burst",
  "data": {
    "field": "...",
    "iterations": 200,
    "wall_clock_ms": <total>,
    "per_event_mean_ms": <number>,
    "per_event_p50_ms": <number>,
    "per_event_p95_ms": <number>,
    "per_event_p99_ms": <number>,
    "per_event_max_ms": <number>,
    "dispatched": <count of edits enqueued>
  }
}
```

The command **queues** N synthetic `PendingFieldEdit` objects into the `UiDrawSession::queuedFieldEdits` deque (the same path the UI uses). Measurement: wall-clock per `PostToMainThread`-style enqueue. The HTTP-driven worker runs asynchronously; the assertion is that **enqueue cost** is below the per-frame budget. Combined with the `perf.snapshot` rows the assertion is end-to-end.

Because the actual HTTP commit requires a live tracker, when `--no-network` is provided the command exercises the enqueue + state-machine + worker dispatch only — the worker hits a stub or short-circuits at `app.Backend == nullptr`. This is sufficient for the regression gate; full E2E (with a live Jira) is a manual-only validation.

## Verification

- `cmake --build --preset ninja-iter-msys2 --target SmatchetStandalone SmatchetCore_DX12` — dual-target green.
- `cmake --build --preset ninja-test-msys2 && ctest --output-on-failure` — green.
- `cmake -B build/ninja-ai-off-check -DSMATCHET_WITH_AI=OFF && cmake --build build/ninja-ai-off-check --target SmatchetStandalone` — AI-OFF green.
- `bash scripts/dev/test-all.sh` — includes new `test-grid-edit-perf-postfix.sh`.
- Burst command on a `--spawn` instance: per-event mean ≤ 6.94 ms, p99 ≤ 16.67 ms, no UI-thread `perf_temp:grid.cell_edit_commit` scope > 6.94 ms.
- `grep -rn 'perf_temp:' Source_Core/` returns zero hits after Phase 8.

## Implementation log

(populated on commit)

## Deviations from plan

(populated on commit)
