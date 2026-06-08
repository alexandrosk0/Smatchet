# Tracker subsystem — agent rules

Scoped rules for `Source/Core/src/Tracker/` (+ the `ITracker*` interface headers in `Source/Core/include/` and the per-backend headers in `Source/Core/include/Tracker/`). Global rules (C++14, dual-target, RAII, logging, perf) stay in the root [`AGENTS.md`](../../../../AGENTS.md). Domain glossary: [`CONTEXT.md`](CONTEXT.md). Orientation / request flow: [`README.md`](README.md).

This is a **strict lint zone** (root `AGENTS.md` § Tiered enforcement zones) — every new violation fails CI on a delta basis.

## Invariants

- **No backend leak into shared interfaces.** Backend-specific code (`Jira*`, `Plane*`, `GitHub*`) must NOT leak into `ITrackerBackend` or its five role interfaces (`ITrackerIssueReader`, `ITrackerConnectivity`, `ITrackerFieldCatalog`, `ITrackerIssueMutations`, `ITrackerCollaboration`). The shared interfaces are backend-agnostic; per-backend behaviour lives behind the concrete client (`JiraClient` / `PlaneClient` / `GitHubClient`) only. (The old monolithic `ITrackerClient` was split into these — never reintroduce that name.)
- **HTTP only through `TrackerHttpClient` / `TrackerHttpUtils`.** Flag any direct `cpr::Get` / `cpr::Post` / `cpr::Put` / `cpr::Patch` / `cpr::Delete` in a backend or feature file. Routing through the typed client gets retry-on-transient, error classification (`TrackerError`), and `NetworkUsageTracker` logging for free.
- **Field-value flow is catalog → parser → payload.** Field reads resolve through the field catalog (`ITrackerFieldCatalog` / `FieldCatalogCache` / `TrackerFieldCatalogPure`); values parse through `TrackerFieldValueParser`; writes build through `TrackerFieldPayload` / `TrackerFieldPayloadPure`. Flag any path that bypasses a stage (e.g. hand-rolling field JSON instead of `BuildFieldPayload`).
- **Writes wire to the offline queue + audit trail.** Issue creates and field edits must enqueue through `OfflineQueueService` (owned by Sync — see `Source/Core/src/Sync/`) so an offline edit replays, and must emit a `BackendAuditTrail` begin/result pair attributed via `FieldEditAuditSource` (owned by Persistence). A write that hits the backend without both is a correctness gap, not a style nit.
- **`UpdateField` is set-replace.** `ITrackerIssueMutations::UpdateField(issueId, field, values)` (returns `TrackerError` since #21b Slice 4) takes the **intended full set** for multi-value fields (labels/assignees/components), not a delta. Jira/Plane are natively set-replace; GitHub reconciles internally (pre-fetch + `LabelEditDiffPure` diff) so callers still see one set-replace virtual.

## Before you edit

- Read [`CONTEXT.md`](CONTEXT.md) for the term roster (what `TrackerIssueKey`, `IssueDraft`, `TrackerFieldFamily`, fixture-vs-live, etc. mean) and [`README.md`](README.md) for how a create/update request flows through the layers.
- A new field type touches the whole catalog → parser → payload chain plus each backend's mapper (`JiraIssueMappingPure` / `PlaneIssueMappingPure` / `GitHubIssueSearchMapping`). Don't stop at the catalog.
