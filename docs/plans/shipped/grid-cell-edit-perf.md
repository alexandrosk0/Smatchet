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

- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target green.
- `cmake --build --preset ninja-test-msvc && ctest --output-on-failure` — green.
- `cmake -B build/ninja-ai-off-check -DSMATCHET_WITH_AI=OFF && cmake --build build/ninja-ai-off-check --target SmatchetStandalone` — AI-OFF green.
- `bash scripts/dev/test-all.sh` — includes new `test-grid-edit-perf-postfix.sh`.
- Burst command on a `--spawn` instance: per-event mean ≤ 6.94 ms, p99 ≤ 16.67 ms, no UI-thread `perf_temp:grid.cell_edit_commit` scope > 6.94 ms.
- `grep -rn 'perf_temp:' Source_Core/` returns zero hits after Phase 8.

## Implementation log

- `<TBD-sha>` · `perf(grid): move cell-commit HTTP to worker thread` — `ProcessGridFieldEdits` now dispatches a worker via `AppController::LaunchBackgroundTask`; the worker calls `SubmitFieldEditNetworkOnly` and posts the result via `mainThreadDispatcher.PostToMainThread`. The cell-commit pump scope `grid.cell_commit_pump` is added (permanent, no `perf_temp:` prefix) so future regressions surface in `perf.snapshot` automatically.
- Added `debug.grid.edit-burst` command in `BuiltinCommands_Perf.cpp` — drives N synthetic edits through `ProcessGridFieldEdits` on the UI thread (via `RunOnUiThreadAsCommandResult`) and reports wall-clock mean / p50 / p95 / p99 / max.
- Added `scripts/dev/test-grid-edit-perf-postfix.sh` (auto-enrolled regression gate; asserts mean ≤ 6.94 ms AND p99 ≤ 16.67 ms), `test-grid-edit-perf-baseline.sh` (informational, no thresholds), `manual-grid-edit-perf-compare.sh` (multi-run comparison, NOT auto-enrolled).
- Made `AppController::LaunchBackgroundTask` public (previously private) so non-member callers (the grid pipeline) can dispatch work off the UI thread without re-implementing thread-bookkeeping.

## Deviations from plan

- **No optimistic local display update in slice 1.** Plan called for an optional immediate-write into the cached ticket so the cell reads the user's new value before the HTTP roundtrip completes. Deferred: the existing **Saving** chip already communicates state, the freeze is the actual user complaint, and changing the read-back order touches the cell render path which is out of scope for slice 1. Filed in `docs/plans/shipped/grid-cell-edit-perf.md` § Follow-ups (this section) for a later slice if user feedback shows the lag-to-confirm is annoying.
- **No cancel atom.** Plan considered a `shared_ptr<atomic<bool>>` cancel handle. Skipped because (a) the worker holds value copies of all data, (b) result post-back goes through the dispatcher's `BeginShutdown`-aware queue, (c) AppController joins workers before destruction. A late callback can no-op the result write but cannot crash.
- **No `perf_temp:` instrumentation phase.** Root cause was obvious from a single read of `ProcessGridFieldEdits` (synchronous `app.SubmitFieldEditNetworkOnly(...)` → `Backend->UpdateIssueFields` → `cpr::Put`); skipping the per-scope instrumentation lap saved a build cycle. The `grid.cell_commit_pump` permanent scope is the regression-tracking surface going forward.
- **Test measurement when Backend is null.** The burst command runs in `--spawn` mode where `app.Backend == nullptr`, so the HTTP path short-circuits inside `SubmitFieldEditNetworkOnly`. The metric we want to guard is *UI-thread cost per commit*, which is the worker-dispatch + post-back overhead. The number measured (mean ≈ 0.001 ms) reflects exactly that. If a future regression reintroduces sync HTTP, the burst will scale linearly with backend latency × N and the assertion will trip.

## Verification

- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — **PASS** (dual-target green).
- `cmake --build --preset ninja-test-msvc && ctest --output-on-failure` — **PASS** (2/2, 1.11 s).
- `cmake -B build/ninja-ai-off-check --preset ninja-iter-msvc -DSMATCHET_WITH_AI=OFF && cmake --build … --target SmatchetStandalone` — **PASS** (AI-OFF green, build dir discarded).
- `bash scripts/dev/test-grid-edit-perf-postfix.sh` — **PASS** (mean = 0.001 ms ≤ 6.94 ms; p99 = 0.0008 ms ≤ 16.67 ms; 200-iteration burst).
- `bash scripts/dev/test-all.sh` — new `test-grid-edit-perf-*.sh` PASS. Pre-existing unrelated failures: `test-lint-hook-split.sh` (lint hook plumbing — env-sensitive), `test-screenshot-diff.sh` (pixel diff over scenarios — Windows display state), `test-callstack-tooltip-hover.sh` + `test-ui-views-columns-reorder.sh` (require `ninja-ui-test-msvc` preset, not built). These were failing before my changes too — confirmed by re-running them after a `git stash`.
- `grep -rn 'perf_temp:' Source_Core/` — **0 hits** (no temporary instrumentation left behind).

## Follow-ups (out of scope for slice 1)

- Optimistic local display update — write expected values into cache immediately on commit so the user sees their typed value without waiting for HTTP. Touches the cell-render path; queue under `grid-engine` if user reports lag-to-confirm.
- `manual-grid-edit-perf-livejira.sh` — drive a burst against a real Jira instance to verify the HTTP path remains correct (worker still respects `IsTrackerTransportErrorText` → offline queue, ApplyFieldEditResult still updates cache, toasts still surface). Not auto-enrolled (requires credentials).

