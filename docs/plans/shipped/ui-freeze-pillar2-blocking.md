# Plan — Pillar-2 UI-thread blocking elimination (future-destructor + sync-I/O cluster)

> **Slug**: `ui-freeze-pillar2-blocking` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — all cited PRs merged (see Implementation log); archived 2026-06-16 via plan-archival sweep.

## Context

A cluster of open GitHub Issues all violate **UX Pillar 2** (no UI-thread block > 100 ms without a visible cue). Two mechanisms:

- **(A) Blocking `std::future` destructors on the UI thread** — destroying a `future`/`vector<future>` whose worker is still running blocks until the worker drains.
  - **#734** (P2) — `bulkImportFutures.clear()` at `SmatchetBulkTicketsUi.cpp:132/226/311` blocks during *normal operation*.
  - **#1150** (P3) — `SmatchetUserInfoUi`'s 4 future members (`vcsFuture_`/`activityFuture_`/`groupsFuture_`/`membersFuture_`, `SmatchetUserInfoUi.h:108-111`) block at *app shutdown* (p4 annotate / network can stall teardown several seconds). NB: the block is currently the *safety mechanism* keeping the worker's captured `appPtr` valid (`SmatchetUI Ui` is destroyed before `AppController`) — any fix must preserve that no-UAF guarantee.
- **(B) Sync I/O reaching the ImGui render path** — **#1001** tracking issue, 5 sites alive on develop:
  - **#611** `SmatchetToolbarUi.cpp` RefreshTrackerAppendCache → `LoadPersistentViewsFromDisk` (ifstream+JSON under IoMutex+ScopedFileLock) from RenderBar (memoized; blocks on the memo-miss frame).
  - **#761** `AnnotateAnalysisUi_Window.cpp` DrawCallstackProcessControls runs `p4 changes -r -m1 //...@a,b` synchronously on confirm.
  - **#732** `SmatchetPreferencesUi_Templates.cpp` duration/work-log sub-tabs call `ConfigManager::Save` (RMW + DPAPI + disk under 2 mutexes) synchronously per reorder/delete/add click.
  - **#767** `SmatchetViewsDashboardUi_widgets.cpp` `ListCachedProjects()` (ifstream+parse+migrate+sort) every frame the project-pill popup is open.
  - **#892** `SmatchetPreferencesUi.cpp` DrawTrackerRecentProjects → `ListCachedProjects()` every frame the Tracker tab is open.

Intended outcome: after this lands, no UI-thread frame blocks on future-drain or sync I/O for these paths; each offloaded path shows a visible in-progress cue.

## Approach

Two workstreams under one plan (shared Pillar-2 goal + the same accepted offload toolkit), shippable as **separate PRs** (different subsystems, no shared seam beyond the helper).

**WS-A — cooperative future cancellation (decided 2026-06-13).** Each UI-owned worker takes a `stop_token`-style cancel flag (a shared `std::atomic<bool>` / a small `CancelToken` struct) and checks it at its await/loop/IO-chunk boundaries, returning promptly when set. The UI owner, on `.clear()` (#734) / window-close / shutdown (#1150), *signals cancel* then abandons the future without an inline blocking wait — the worker observes the flag and exits fast, so the eventual join (kept at `AppController` teardown, mirroring `DrainUiDrawSessionFuturesBeforeAppTeardown`) is near-instant. This fixes **both** the during-run block (#734) **and** the shutdown stall (#1150, several-second p4/network drain), at the accepted cost of touching every UI-worker body to add the cancel-check. The no-UAF guarantee (Pillar-3) is preserved: teardown still joins before `AppController` dies, and a cancelled worker that captured `appPtr` returns before touching it post-signal. A small `CancelToken` + an owner-side `CancellableFutureSet` helper (signal-all + non-blocking abandon + teardown join) is the reusable seam; per-worker bodies add one cooperative check.

**WS-B — sync-I/O offload audit.** Apply the three already-accepted in-tree patterns site-by-site: `snapshot-on-open` into `UiDrawSession` (#767, #892 — the per-frame `ListCachedProjects()` reads), `MarkPrefsDirty` deferred-save (#732 — match the sibling tabs that already defer), `LaunchBackgroundTask`+`PostToMainThread` with a spinner cue (#761 p4 round-trip; #611 memo-miss `LoadPersistentViewsFromDisk`). No new pattern invented. **Batched as 3 PRs by pattern** (decided): snapshot-on-open (#767/#892) · MarkPrefsDirty (#732) · LaunchBackgroundTask+cue (#761/#611).

**Sequencing (decided): WS-A and WS-B ship in parallel** — independent files; WS-B does not consume the WS-A cancel helper.

## Files to modify

WS-A (cooperative cancellation):
1. `Source/Core/include/Ui/CancelToken.h` (+ `.cpp` if needed) — new `CancelToken` (shared atomic flag) + `CancellableFutureSet` owner helper (signal-all, non-blocking abandon, teardown join). Grep-confirm absent before naming.
2. `Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp:132/226/311` — signal-cancel + non-blocking abandon instead of blocking `.clear()`; the bulk-import worker body checks the token between rows.
3. `Source/Core/include/Ui/SmatchetUserInfoUi.h:108-111` + its `.cpp` — give the 4 fetches a `CancelToken`; signal on window-close/shutdown; the vcs/activity/groups/members worker bodies check the token at their IO boundaries (esp. the p4-annotate activity scan — the #1150 multi-second case).
4. `Source/Core/src/Ui/SmatchetUI_Layout.cpp:248` (`DrainUiDrawSessionFuturesBeforeAppTeardown`) — signal-all-cancel before the join so teardown drains fast.
5. **Each touched worker body** — add the cooperative cancel-check at its await/loop/IO-chunk boundary (the accepted cost of this approach).

WS-B (one site per row; each its own commit, batchable):
5. `SmatchetViewsDashboardUi_widgets.cpp` (#767) + `SmatchetPreferencesUi.cpp` (#892) — snapshot `ListCachedProjects()` on popup/tab open into `UiDrawSession`.
6. `SmatchetPreferencesUi_Templates.cpp` (#732) — swap sync `ConfigManager::Save` for `MarkPrefsDirty`.
7. `AnnotateAnalysisUi_Window.cpp` (#761) + `SmatchetToolbarUi.cpp` (#611) — `LaunchBackgroundTask`+`PostToMainThread` + spinner cue.

## Existing utilities reused

- `DrainUiDrawSessionFuturesBeforeAppTeardown` (`SmatchetUI_Layout.cpp:248`, decl `SmatchetUI.h:70`) — the shutdown-join model WS-A's graveyard drainer mirrors.
- `AppController::LaunchBackgroundTask` + `MainThreadDispatcher::PostToMainThread` — the worker-offload primitive (joined at shutdown via `JoinBackgroundTasks`).
- `MarkPrefsDirty` deferred-save (already used by sibling Preferences tabs) — the #732 fix.
- `UiDrawSession` snapshot fields — the snapshot-on-open target for #767/#892.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: WS-B removes per-frame `ListCachedProjects()` disk reads (#767/#892) from the hot popup/tab path — net steady-state improvement. No new per-frame cost (snapshot is read once on open).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: the entire point. WS-A removes future-destructor blocks; WS-B removes sync I/O from the render path. Every offloaded long op (#761 p4, #611 view-load) gets a spinner/in-progress cue.
- **Pillar 3 (never crash)**: WS-A must preserve the no-UAF guarantee — the graveyard drainer is joined before `AppController` teardown so a worker's captured `appPtr` never dangles. Sanitizer build must stay clean. This is the highest-risk area (lifetime).
- **Pillar 4 (accessibility)**: N/A — no new interactive surface (spinners are non-interactive cues).

## Perf-review-system gates (mandatory — diff touches `Source/Core/`)

1. **PR-fast CI** — scenarios: `bulk-import` (WS-A #734), `preferences-*` (#732/#892), `views-dashboard` (#767), `toolbar-*` (#611). Map each in `perf-gatekeeper.md` § Curated diff → scenario map; declare the subset in `perf-pr-fast-set.json` per PR.
2. **Pillar 2 static scanner** — WS-B explicitly *removes* sync-I/O-reachable-from-`ImGui::*` sites; the scanner should go from flagging these to clean. WS-A: confirm no new sync I/O reachable from a draw fn.
3. **Dispatcher drain** — WS-A interacts with `MainThreadDispatcher::Drain()` (PostToMainThread completions) and the teardown join — exercise both.
4. **Visible-cue bucket-E harness** — #761/#611 add a >100 ms offloaded path → each needs a visible-cue bucket-E test asserting the spinner renders while the worker runs.
5. **Marker inventory** — if any `SMATCHET_UI_PERF_SCOPE` markers are added, regen `docs/perf/MARKER_INVENTORY.md` in the same PR.

**Pre-push**: run `perf-workflow.md` § Gate-check vs baseline against the named scenarios before each PR.

## Risks / non-goals

- **Risk (Pillar-3, WS-A lifetime — now LARGER surface)**: cooperative cancellation touches every UI-worker body, so the no-UAF guarantee must hold per worker — a cancelled worker must observe the flag and return *before* dereferencing captured `appPtr`/`this` again. Mitigation: the teardown join is kept (signal-all → join is fast, not removed); each touched worker gets an ASan-exercised cancel path; the WS-A PR runs the sanitizer bucket with the #1150 mid-fetch-shutdown repro. Higher-risk than the graveyard alternative (which the grill rejected in favour of also fixing shutdown) — review each worker's post-signal access carefully.
- **Risk**: WS-B snapshot-on-open can show stale data if the underlying file changes while open. Accepted — these are cached-project / view lists; a refresh-on-reopen is fine (document it).
- **Non-goal**: the inverse-asymmetry tracker findings (#943-related) — different subsystem, separate PR (already in flight as the #943/#984 fix PR).
- (Cooperative cancellation is now IN scope per the grill — no longer deferred.)

## Verification

- **Bucket A (pure-logic ctest)**: `UiFutureGraveyard` move/drain semantics (a stub future that signals when waited) — assert move-in is non-blocking, drain joins.
- **Bucket E (ImGui Test Engine)**: visible-cue tests for #761/#611 (spinner renders while worker runs); a #734 test that `bulkImportFutures` clear returns within one frame budget while a stub worker is still "running".
- **Bash-driver / sanitizer**: ASan bucket run on the WS-A PR (Pillar-3 lifetime) — no UAF at simulated mid-fetch shutdown (#1150 repro: trigger a User Info fetch, close the app, assert clean teardown).
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: grill the open design questions below with the user before WS-A implementation. Required — do not delete.
- **Manual residue**: none expected; #1150's mid-fetch-shutdown repro is automatable via the sanitizer bucket above.

## Resolved decisions (grilled 2026-06-13)

1. **WS-A shape** — **full cooperative cancellation now** (stop-token each worker polls), not the graveyard-only approach. Fixes the shutdown stall (#1150) as well as the during-run block (#734); accepted cost = touching every UI-worker body.
2. **#1150 shutdown** — fix now (covered by the cancellation in WS-A).
3. **WS-B batching** — split by pattern → 3 PRs (snapshot-on-open #767/#892 · MarkPrefsDirty #732 · LaunchBackgroundTask+cue #761/#611).
4. **Sequencing** — WS-A and WS-B in parallel (independent).

## Out of scope (flagged, not designed)

**Deferral residue-sweep (performed 2026-06-13; re-run before merge if the plan changes)** — grepped `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for stray refs to anything deferred here. The only stray was an in-doc one: a stale "cooperative cancellation deferred" line that survived the grill pivot — now removed, since cooperative cancellation is **in WS-A scope** (Approach + Resolved decision #1). No external stray refs.

- A general Pillar-2 static-scanner expansion to catch future-destructor blocks (not just sync I/O) — possible tooling follow-up.

## Implementation log

**WS-A — cooperative future cancellation (shipped 2026-06-13).** WS-B (#1001 cluster) shipped separately as #1181; this entry covers WS-A only.

- **`Source/Core/include/CancelToken.h`** (new, header-only) — `smatchet::ui::CancelToken` (copyable handle onto a shared `std::atomic<bool>`; `Cancel` / `IsCancelled` / `Reset`, fail-safe-to-cancelled on a null handle) + `smatchet::ui::CancellableFutureSet<T>` owner helper (`Token` / `Add` / `SignalCancelAll` / `AbandonInto` non-blocking / `JoinAll` teardown drain). Dual-target-safe (only `<atomic>/<future>/<memory>/<vector>`); no GLFW/GL.
- **`SmatchetBulkTicketsUi.cpp`** (#734) — the three `bulkImportFutures.clear()` sites (parse / close / run) now call a new `BulkImportAbandonFutures(d)` = signal `d.bulkImportCancel.Cancel()` → move still-valid futures into `d.bulkImportFutureGraveyard` → `Reset()` the token. The per-row create passes `d.bulkImportCancel` to `CreateIssueAsync`.
- **`AppController::CreateIssueAsync`** — gained a defaulted `smatchet::ui::CancelToken cancel` param (Lua caller unaffected); the worker checks `IsCancelled()` before the network create (returns a benign "Cancelled." result) and again before the post-create `RefreshLocalData` / hydration that dereferences `this`.
- **`SmatchetUiSession.h`** — added `bulkImportCancel` (token) + `bulkImportFutureGraveyard` to `UiDrawSession`.
- **`SmatchetUserInfoUi.{h,cpp}`** (#1150) — added `cancel_` (shared by the 4 std::async workers) + `appForShutdownCancel_`. Each worker captures a token copy and checks `IsCancelled()` at its IO boundary before dereferencing the captured raw `appPtr`. New `~SmatchetUserInfoUi()` signals `cancel_` + calls `ClearPaneUserActivity(paneId_)` (the backend's in-scan IO cancel — the multi-second p4 case) BEFORE the future members destruct, so the kept destructor-join is near-instant. `closeCleanup` / `adoptPendingRequest` signal+`Reset` the token.
- **`SmatchetUI_Layout.cpp`** `DrainUiDrawSessionFuturesBeforeAppTeardown` — signals `d.bulkImportCancel.Cancel()` before the join and also drains `d.bulkImportFutureGraveyard` (abandoned-but-running creates from earlier `.clear()` calls).

**Pillar-3 (no-UAF) reasoning confirmed:** in `BootstrapContext` (StandaloneAppBootstrap.h) `app` is declared before `mainWindow`, so `~SmatchetUI` (and thus `~SmatchetUserInfoUi`) runs BEFORE `AppController` is destroyed — the captured `appPtr` stays valid across the fast signal-then-join, and a cancelled worker returns before touching it again. The teardown join is KEPT (signal → join), never removed.

## Deviations from plan

- **Bulk-import keeps the indexed `std::vector<std::future>` rather than adopting `CancellableFutureSet` directly.** The bulk pump addresses futures by row index (`bulkImportFutures[idx]`), incompatible with the set's opaque membership; instead a sibling `CancelToken` + graveyard pair replicates the set's signal/abandon/drain semantics over the existing indexed vector. `CancellableFutureSet` is still the reusable seam (covered by bucket-A) and is the right fit for non-indexed owners.
- **Bulk-import `CreateIssueAsync` does NOT have a blocking future destructor** (it is a `LaunchBackgroundTask` + promise, not a `std::async`), so the literal #734 "blocking `.clear()`" is a near-no-op on destruction — but the abandoned worker would still run the full network create + post-create refresh. The WS-A fix makes that worker short-circuit on cancel (skipping the network create AND the expensive refresh), which is the substantive Pillar-2 win the issue is about. The non-blocking-abandon shape is still applied per the plan.
- **Test-infrastructure finding (filed):** the `ninja-msvc-asan` preset historically instrumented only the app targets, never the `SmatchetTests` doctest rig — a heap UAF in a test TU compiled uninstrumented and could not trip ASan. WS-A added an opt-in `SMATCHET_SANITIZE_TESTS` knob (default OFF) so the #1150 repro runs genuinely sanitized; full-rig ASan is blocked by a pre-existing `/RTC1`-vs-`/fsanitize=address` death-test SIGABRT, filed as a tooling follow-up.

## Verification (actual)

- **Bucket A (pure-logic ctest)** — `tests/Core/CancelToken.test.cpp`: 7 cases / 24 assertions, green (token share/idempotent/reset semantics; set add/abandon/join; cooperative early-out; shutdown-drain swallows worker exception). ASan-clean.
- **Bucket E (#734)** — `tests/Core/BulkImportAbandonNonBlocking.test.cpp`: re-creates `BulkImportAbandonFutures(d)` against 4 futures each blocking 3000 ms; asserts the signal-then-abandon returns in `< 250 ms` (vs the ~3 s an inline join would cost). Green.
- **Sanitizer (#1150, Pillar-3)** — `tests/Core/UserInfoActivityCancelUaf.test.cpp`: reconstructs the `launchActivityFetch` UAF shape (worker holds raw controller ptr + token, blocked mid-scan on a latch released by the IO-cancel hook; owner dtor signals cancel + releases latch before the future destructs; controller freed after the owner). Built with `-DSMATCHET_SANITIZE_TESTS=ON` and run under `ASAN_OPTIONS=abort_on_error=1`: UAF-clean, 20/20 stress iterations clean; a deliberate negative control (worker reads freed heap) trips `heap-use-after-free` (exit 99), proving the instrumentation is live.
- **Dual-target build** — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12 SmatchetTests`: all green (Smatchet.exe + SmatchetCore_DX12.lib + SmatchetTests.exe).
- **Full functional suite** — `SmatchetTests.exe`: 1690 cases / 15660 assertions, 0 failed.
- **Lint gate** — `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop`: PASS (only a pre-existing unchanged comment-ratio WARN on AppController.h).
- **Manual residue**: full-stack bucket-E drive of `SmatchetUserInfoUi::DrawWindow` (real ImGui frame + mock blocking backend) and full-rig ASan in CI are deferred with concrete follow-up plans (see Deviations); the faithful doctest analogue covers the Pillar-3 ordering contract today.

*(WS-B already shipped as #1181 — its log/verification lives on that PR.)*
