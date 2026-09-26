# Offline-first is an enforced Quality Pillar via lint gates and code-review rules

**Status:** accepted (2026-09-24)

Offline-first becomes **UX Quality Pillar 6**, enforced like Pillar 5 (DRY) via two **blocking exact rules** and five **WARN-first heuristics**, plus a fake-network test harness and the `OfflineFirst` bucket-E lane.

Evidence:
- **PR #2234 status combo blocked on a live fetch** for 1–90 s offline ("Loading transitions…" with no option to pick anything, even invalid moves).
- **Comments modal ignored cached comments**, sitting on "Loading comments..." despite the cached thread visible in a hover.
- **Jira catalog failures collapsed to `Unknown`** (the #21b TODO in `JiraClient::FetchFieldCatalog`), wiping the catalog, turning the grid read-only, and dropping pending edits.
- **~20 further sites:** status never queueable offline; comments modal false "Comment Queued"; worklog, watch, Annotate, command/Lua field edits and bulk import bypass the offline queue; reads with no offline copy (per-project components, users, editmeta, project picker, attachments, watchers/votes).

**Decision:**
1. **Four invariants** (the Pillar 6 text in [`docs/agent-rules/quality-pillars.md`](../agent-rules/quality-pillars.md) § 6 is canonical):
   - A network-backed read that has cached data renders it with a freshness cue (`DataFreshnessCue`), never a loading-only or empty state. "Loading…" alone is only for `DataFreshness::LoadingNoCache`.
   - A failed fetch never discards cached data and is never remembered as final: it backs off (`KeyedLookupCache`) and retries after reconnect, and its in-flight flag clears on every path.
   - Every tracker write goes through the offline queue when the tracker is unreachable (`RouteWrite` → queue immediately) and replays on reconnect. Toasts say what actually happened ("Queued offline" vs "Saved").
   - A `TrackerError` keeps its `Transport` kind wherever it is flattened; `TrackerErrorUnknown(<string>)` is never how a failure is reported.
2. **One shared pattern** instead of per-feature state machines: `OfflineFirstPure.h`, `KeyedLookupCache.h` + `RunKeyedFetch`, and `DataFreshnessCue`.
3. **Additive SQLite tables** for offline reads and writes: `lookup_cache` (S5) and `pending_actions` (S8).
4. **Split strictness**: the two exact rules **block** now; the five heuristics are **WARN-first** and graduate one by one.
5. **A test seam**: the fake-network harness (`tests/support/FakeNetworkSwitch.h`) and the `OfflineFirst` bucket-E lane.

## Considered options

- **Aspirational only** — a stated § Quality rule, no gate (like Pillar 4 Accessibility). *Rejected*: PR #2234 shipped past review, so a gate is mandatory.
- **Block every rule now** — *Rejected*: the five heuristics have false positives on ImGui boilerplate (loading-only renders without a freshness cue but in a correctly-gated fetch); WARN-first calibration avoids blocking legit PRs.
- **Per-lookup JSON snapshots instead of SQLite** — *Rejected*: many workers upsert learned transitions; JSON files have no atomic-upsert primitive, so SQLite's `INSERT OR REPLACE` is required.

## Consequences

- **New gates**: `offline-write-bypasses-queue` and `tracker-error-kind-collapsed` **block** (delta-gated per changed file; existing hits grandfathered); `offline-loading-only-render`, `offline-inflight-latch-unguarded`, `offline-failure-cached-as-loaded`, `offline-cache-cleared`, `offline-network-read-ungated` **WARN-first** → graduate independently per the trigger below.
- **Gate infra**: `lint-rules.d/72-offline-exact.sh` (blocking rules) and `74-offline-heuristic.sh` (WARN heuristics), loader + `--scan-offline` in `test-lint-rules.sh`, bats coverage, enforcement-contract row in `AGENTS.md`, and `docs/agent-rules/cpp-rules.md` section.
- **Shared primitives**: `Source/Core/include/OfflineFirstPure.h` (pure connectivity, freshness and write-route decisions), `KeyedLookupCache.h` (offline-gated keyed fetch with backoff, reset on reconnect), `DataFreshnessCue.h` (the one localized cue).
- **Additive schema**: SQLite `lookup_cache` (S5) and `pending_actions` (S8) tables for offline reads and writes.
- **Graduation, per WARN rule, independent of the others:**
  - Whole-tree `--scan-offline` hits burned to 0 (fixed or deviation-marked).
  - Fewer than 10% false positives over ~20 PRs touching offline scope, tallied by `code-review` in `docs/high-integrity/offline-calibration.md` (created by the first PR that records a tally).
  - Or a maintainer decision.
  - `offline-write-bypasses-queue` becomes absolute-0 (no exemptions) at the end of S9.
- **Owners**: `offline-sync` is the implementer; `code-review` is the reviewer-of-record for blocking gates + WARN heuristic disposition.
- Full design + slices + root causes: `docs/plans/active/offline-first.md`.
